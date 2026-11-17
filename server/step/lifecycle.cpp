#include "catalog/catalog.hpp"
#include "step.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace amber::step {
namespace {
template <class T> void unique(std::vector<T> &rows) {
  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
}
void changed_relations(react::DeferredEffects &effects,
                       std::span<const space::RelationChange> changes,
                       const std::array<TermId, 4> &terms, const amber::Origin &origin) {
  for (const auto &change : changes) {
    const auto term = terms.at(static_cast<std::size_t>(change.kind));
    if (term) {
      effects.ordered.emplace_back(react::WatchWork{{change.from, term}, origin});
      effects.ordered.emplace_back(react::WatchWork{{change.to, term}, origin});
    }
  }
}
} // namespace

void apply_custody(amber::ObjectState &objects_state, amber::space::State &space_state,
                   std::array<TermId, 4> terms, const perm::Plots &plots,
                   std::span<const perm::CustodyChange> custody, NativeCompletion &out) {

  auto identity = amber::identity(std::as_const(objects_state.objects));

  auto &parts = objects_state.parts;

  const auto changes_begin = space_state.relation_changes.size();
  for (const auto &change : custody) {
    const auto closure = lifecycle::collect(parts, identity, change.goods);
    const auto descendants = std::span(closure).subspan(1);
    const space::VisibleCustody visible{change.goods, change.parent,
                                        change.parent &&
                                            bool(identity.flags[change.parent.slot()] & Body)};
    const auto result =
        change.in_world ? space::visible_custody(objects_state, space_state, visible, descendants)
                        : space::custody(objects_state, change.goods, change.parent, space_state,
                                         descendants, change.stay_static);
    if (result.status != space::Status::Ok)
      throw std::invalid_argument(space::status_text(result.status));
    const auto crossed = perm::update_plots(objects_state.objects, plots, closure, change.in_world);
    out.crossed_plots.insert(out.crossed_plots.end(), crossed.begin(), crossed.end());
    out.changed_objects.insert(out.changed_objects.end(), closure.begin(), closure.end());
    auto &visibility = change.in_world ? out.visible_objects : out.hidden_objects;
    auto &old_visibility = change.in_world ? out.hidden_objects : out.visible_objects;
    std::erase_if(old_visibility, [&](Handle h) {
      return std::find(closure.begin(), closure.end(), h) != closure.end();
    });
    visibility.insert(visibility.end(), closure.begin(), closure.end());
  }
  const auto changes = std::span(space_state.relation_changes).subspan(changes_begin);
  changed_relations(out.effects, changes, terms, {});
  for (const auto &change : changes) {
    out.changed_objects.push_back(change.from);
    out.changed_objects.push_back(change.to);
  }
  space_state.relation_changes.resize(changes_begin);
}

void change_custody(amber::ObjectState &objects_state, amber::space::State &space_state,
                    std::array<TermId, 4> terms, const perm::Plots &plots,
                    IlluminationSources &sources, NativeCompletion &out) {
  auto &space = space_state.space;

  auto identity = amber::identity(std::as_const(objects_state.objects));

  auto &parts = objects_state.parts;

  auto &lights = space_state.lights;
  auto &zones = space_state.zones;
  auto &points = space_state.light_subscriptions;

  const auto custody = std::move(out.custody);
  out.custody.clear();
  std::vector<Handle> hiding;
  for (const auto &change : custody)
    if (!change.in_world) {
      const auto closure = lifecycle::collect(parts, identity, change.goods);
      hiding.insert(hiding.end(), closure.begin(), closure.end());
    }
  unique(hiding);
  // Zone bounds must dirty their old light subscribers before custody removes the sensor.
  retire_illumination(sources, lights, zones, points, space, hiding);
  apply_custody(objects_state, space_state, terms, plots, custody, out);
}

void project_visibility(IlluminationSources &sources, amber::space::State &space_state,
                        net::ReplicationIndex &replication, TransformRead transforms,
                        const amber::catalog::State &catalog_state, const react::ValueRead &values,
                        const NativeCompletion &out, float source_radius_m,
                        const amber::step::Clock &clock) {
  auto &space = space_state.space;

  auto &lights = space_state.lights;
  auto &zones = space_state.zones;
  auto &points = space_state.light_subscriptions;
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;

  retire_illumination(sources, lights, zones, points, space, out.hidden_objects);
  for (auto object : out.hidden_objects)
    net::retire_replication(replication, object);
  register_illumination(sources, space_state, transforms, catalog_state, values,
                        out.visible_objects, source_radius_m, clock);
  for (auto object : out.visible_objects)
    net::register_replication(replication, object, values.identity, transforms, models, parameters,
                              assets, values);
}

void retire_commerce(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                     std::span<const Handle> removed, Step now, NativeCompletion &out) {
  auto &ledger = perm_state.ledger;
  auto &offers = perm_state.offers;
  auto &trades = perm_state.trades;
  auto &grants = perm_state.grants;
  auto &players = perm_state.players;
  auto &owner_limits = perm_state.owner_limits;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto flags = amber::permission_flags(objects_state.objects);

  auto cancellation = retire_goods(offers, trades, identity, flags, removed);
  out.changed |= cancellation.changed;
  out.changed_objects.insert(out.changed_objects.end(), cancellation.changed_objects.begin(),
                             cancellation.changed_objects.end());
  out.records.insert(out.records.end(), std::make_move_iterator(cancellation.records.begin()),
                     std::make_move_iterator(cancellation.records.end()));
  for (const auto &refund : perm::refund_object_accounts(ledger, authority, removed)) {
    record::Event event;
    event.step = now;
    event.kind = record::Kind::System;
    event.name = "account_refund";
    event.changed = true;
    event.result = record::Result::Applied;
    event.things = {refund.object};
    event.detail = {{"from", perm::account_name(perm::Account::object(refund.object))},
                    {"to", perm::account_name(perm::Account::player(refund.owner))},
                    {"amount", refund.amount},
                    {"agent", false}};
    out.records.push_back(std::move(event));
  }
  perm::revoke_objects(grants, players, removed);
  for (auto object : removed) {
    const auto owner = authority.owner[object.slot()];
    if (owner < players.rows.size()) {
      std::erase(players.rows[owner].inventory, object);
      std::erase(players.rows[owner].mailbox, object);
    }
    owner_limits.erase(object.slot());
  }
}

void retire_physics(space::Space &space, amber::ObjectState &objects_state,
                    space::PhysicalRelations &physical, amber::step::State &step_state,
                    std::span<const Handle> removed, std::array<TermId, 4> terms,
                    const amber::Origin &origin, NativeCompletion &out) {
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto parents = amber::parents(objects_state.objects);
  auto flags = amber::space_flags(objects_state.objects);
  auto &intents = step_state.intents;
  auto &intent_endings = step_state.intent_endings;

  std::vector<space::RelationChange> changes;
  for (auto object : removed) {
    if (auto ending = cancel_move(intents, object))
      intent_endings.push_back(std::move(*ending));
    space::remove(space, identity, object, parents, flags, physical, changes);
  }
  changed_relations(out.effects, changes, terms, origin);
  for (const auto &change : changes) {
    out.changed_objects.push_back(change.from);
    out.changed_objects.push_back(change.to);
  }
}

void retire_publication(net::ReplicationIndex &replication, catalog::InstanceTexts &texts,
                        std::span<const Handle> removed) {
  for (auto object : removed) {
    net::retire_replication(replication, object);
    texts.rows.erase(object.bits);
  }
}

void refresh_object_plots(const space::Space &space, const ObjectState &objects_state,
                          react::State &react_state, const catalog::State &catalog_state,
                          const Clock &clock, const amber::Origin &origin, NativeCompletion &out) {
  const auto values = react::value_read(objects_state, catalog_state, react_state);
  const auto identity = values.identity;

  out.changed_objects.insert(out.changed_objects.end(), out.visible_objects.begin(),
                             out.visible_objects.end());
  out.changed_objects.insert(out.changed_objects.end(), out.hidden_objects.begin(),
                             out.hidden_objects.end());
  unique(out.changed_objects);
  std::erase_if(out.changed_objects, [&](Handle object) { return !identity.valid(object); });
  out.visible_objects.clear();
  out.hidden_objects.clear();
  for (auto object : out.changed_objects) {
    const bool visible =
        space::body_motion(space, identity, object).has_value() ||
        (space::is_logical_root(space, identity, object) &&
         (!(identity.flags[object.slot()] & Held) || space::bounds(space, identity, object)));
    (visible ? out.visible_objects : out.hidden_objects).push_back(object);
  }
  unique(out.crossed_plots);
  std::erase_if(out.crossed_plots, [&](Handle object) { return !identity.valid(object); });
  react::refresh_plot_settings(values, react_state, catalog_state.scripts, out.crossed_plots,
                               clock.step, clock.hz, origin, out.effects);
}

void refresh_rule_reactions(const ObjectState &objects_state, const catalog::State &catalog_state,
                            const react::State &react_state, space::State &space_state,
                            step::State &step_state, std::span<const ScriptId> changed) {
  if (changed.empty())
    return;
  const auto values = react::value_read(objects_state, catalog_state, react_state);
  const auto &scripts = catalog_state.scripts;
  refresh_rule_thresholds(step_state.timers, react_state, values, scripts, changed,
                          step_state.clock);
  const auto illuminance =
      catalog_state.mechanical.values.at("measurements").at("illuminance").get<TermId>();
  bool affected = false, discover = false;
  for (auto script : changed) {
    const auto &declaration = scripts.rows.at(script).declaration;
    if (declaration.kind == react::ScriptDeclaration::Kind::Rule &&
        std::any_of(declaration.watches.begin(), declaration.watches.end(),
                    [&](const auto &watch) { return watch.term == illuminance; })) {
      affected = true;
      discover |= declaration.active;
    }
  }
  if (affected)
    refresh_illuminance_reads(space_state.light_subscriptions, react_state, values, scripts,
                              transforms(objects_state.objects), illuminance, step_state.clock,
                              discover);
}

void publish_catalog_changes(net::Sessions &sessions, const catalog::Models &models,
                             const catalog::ModelTexts &texts, const react::Terms &terms,
                             const react::Materials &materials,
                             net::MaterialAppearanceRoles appearance, double world_ambient_lux,
                             NativeCompletion &out) {
  unique(out.changed_models);
  unique(out.changed_terms);
  unique(out.changed_materials);
  if (out.world_ambient_changed)
    sessions.broadcast({{"t", "catalog"}, {"world", {{"ambient_lux", world_ambient_lux}}}});
  if (!out.changed_models.empty())
    sessions.broadcast({{"t", "catalog"},
                        {"world", {{"ambient_lux", world_ambient_lux}}},
                        {"models", net::model_metadata(models, texts, out.changed_models)}});
  if (!out.changed_terms.empty())
    sessions.broadcast({{"t", "catalog"},
                        {"world", {{"ambient_lux", world_ambient_lux}}},
                        {"terms", net::term_metadata(terms, out.changed_terms)}});
  if (!out.changed_materials.empty())
    sessions.broadcast({{"t", "catalog"},
                        {"world", {{"ambient_lux", world_ambient_lux}}},
                        {"materials", net::material_metadata(terms, materials, appearance,
                                                             out.changed_materials)}});
}

void publish_object_changes(net::Sessions &sessions, IdentityRead identity, AuthorityRead authority,
                            ParentRead parents, const catalog::State &catalog,
                            std::span<const Handle> changed) {
  std::vector<Handle> objects(changed.begin(), changed.end());
  unique(objects);
  std::erase_if(objects, [&](Handle object) { return !identity.valid(object); });
  if (objects.empty())
    return;
  sessions.broadcast(
      {{"t", "metadata"},
       {"objects", net::object_metadata(identity, authority, parents, catalog, objects)}});
}

} // namespace amber::step
