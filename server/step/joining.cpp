#include "run_private.hpp"
#include "start_private.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace amber::step {
namespace {
std::string_view trim(std::string_view name) {
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())))
    name.remove_prefix(1);
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
    name.remove_suffix(1);
  return name;
}
bool same_name(std::string_view a, std::string_view b) {
  a = trim(a);
  b = trim(b);
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
    return std::tolower(x) == std::tolower(y);
  });
}
bool newcomer_available(const catalog::State &catalog, const nlohmann::json &newcomer,
                        ModelId model) {
  const auto script = catalog.models.rows.at(model).script;
  if (!script)
    return false;
  const auto &declaration = catalog.scripts.rows.at(script).declaration;
  if (declaration.kind != react::ScriptDeclaration::Kind::Model)
    return false;
  std::set<std::string_view> configured;
  for (const auto &choices : newcomer.at("body_installs"))
    for (const auto &choice : choices)
      configured.insert(choice.at("slot").get_ref<const std::string &>());
  return configured.size() == declaration.slots.size() &&
         std::ranges::all_of(declaration.slots,
                             [&](const auto &slot) { return configured.contains(slot); });
}
} // namespace
JoinResult create_join(const net::JoinRequest &request, net::Profiles &profiles,
                       perm::State &permissions, ObjectState &objects,
                       const catalog::State &catalog, const nlohmann::json &newcomer, ModelId model,
                       const Clock &clock, const space::State &space) {
  JoinResult result;
  const auto name = trim(request.name);
  const auto characters = std::count_if(name.begin(), name.end(),
                                        [](unsigned char byte) { return (byte & 0xc0) != 0x80; });
  if (name.empty() || characters > 32) {
    result.refusal = "bad_input";
    return result;
  }
  if (std::ranges::any_of(profiles.rows, [&](const auto &profile) {
        return profile.id && same_name(name, profile.name);
      })) {
    result.refusal = "name_taken";
    return result;
  }
  if (!net::join_allowed(request) || !newcomer_available(catalog, newcomer, model)) {
    result.refusal = "refused";
    return result;
  }
  const auto &asset = catalog.assets.rows.at(catalog.models.rows.at(model).asset);
  const auto size = asset.maximum - asset.minimum;
  const auto position =
      space::clear_character_position(space.space, {}, .5f * std::max(size.x, size.y), size.z);
  result.player = static_cast<PlayerId>(profiles.rows.size());
  result.key = net::generate_key();
  permissions.players.rows.resize(std::size_t(result.player) + 1);
  ObjectInit init;
  init.position = position;
  init.owner = init.maker = result.player;
  init.created_step = clock.step;
  init.flags = Body;
  auto created = start::install_object(catalog, objects, model, init);
  perm::update_plots(objects.objects, permissions.plots, created.objects, true);
  NativeCall call;
  call.request.caller = result.player;
  call.request.target = created.root;
  call.body = created.root;
  call.step = clock.step;
  call.hz = clock.hz;
  start::provision_body_grants(newcomer.at("body_installs"), {}, catalog, objects, permissions,
                               call);
  net::Profile profile;
  profile.id = result.player;
  profile.name = name;
  profile.body = created.root;
  profile.key_hashes.push_back(net::hash_key(result.key));
  net::put_profile(profiles, std::move(profile));
  result.objects = std::move(created.objects);
  return result;
}
react::DeferredEffects activate_join(const catalog::State &catalog, react::State &react,
                                     ObjectState &objects, space::State &space, net::State &net,
                                     State &step, const amber::Settings &settings,
                                     std::span<const Handle> created) {
  install_object_physics(step, catalog, react, objects, space, created,
                         {settings.default_friction, settings.default_restitution});
  install_declared_attachments(objects, space, created);
  const auto values = react::value_read(objects, catalog, react);
  const auto poses = amber::transforms(std::as_const(objects.objects));
  const auto &clock = step.clock;
  react::DeferredEffects effects;
  react::install_reactions(catalog, react, values, created, clock.step, clock.hz, {}, effects);
  const auto illuminance =
      catalog.mechanical.values.at("measurements").at("illuminance").get<TermId>();
  sync_illuminance_reads(space.light_subscriptions, react, values, catalog.scripts, poses,
                         illuminance, created, clock);
  register_illumination(step.illumination_sources, space, poses, catalog, values, created,
                        settings.light_source_radius_m, clock);
  for (auto object : created)
    net::register_replication(net.replication, object, values.identity, poses, catalog.models,
                              catalog.parameters, catalog.assets, values);
  const auto start = catalog.mechanical.values.at("events").at("start").get<TermId>();
  for (auto object : created)
    effects.ordered.emplace_back(react::EventWork{object, {}, start, {}, {}});
  return effects;
}
} // namespace amber::step
