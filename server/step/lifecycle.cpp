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
                       const std::array<TermId, 4> &terms, const record::Origin &origin) {
  for (const auto &change : changes) {
    const auto term = terms.at(static_cast<std::size_t>(change.kind));
    if (term) {
      effects.ordered.emplace_back(WatchWork{{change.from, term}, origin});
      effects.ordered.emplace_back(WatchWork{{change.to, term}, origin});
    }
  }
}
} // namespace

void apply_custody(space::Space &space, IdentityRead identity, TransformWrite transforms,
                   ParentWrite parents, SpaceFlags flags, space::PhysicalRelations &physical,
                   const lifecycle::Parts &parts, std::array<TermId, 4> terms, PlotWrite membership,
                   const perm::Plots &plots, std::span<const perm::CustodyChange> custody,
                   NativeCompletion &out) {
  std::vector<space::RelationChange> changes;
  for (const auto &change : custody) {
    const auto closure = lifecycle::collect(parts, identity, change.goods);
    const auto descendants = std::span(closure).subspan(1);
    const space::VisibleCustody visible{change.goods, change.parent,
                                        change.parent &&
                                            bool(identity.flags[change.parent.slot()] & Body)};
    const auto result = change.in_world
                            ? space::visible_custody(space, identity, transforms, parents, flags,
                                                     physical, visible, changes, descendants)
                            : space::custody(space, identity, change.goods, change.parent, parents,
                                             flags, physical, changes, descendants);
    if (result.status != space::Status::Ok)
      throw std::invalid_argument(space::status_text(result.status));
    perm::update_plots(identity, transforms, membership, plots, closure, change.in_world);
    out.changed_objects.insert(out.changed_objects.end(), closure.begin(), closure.end());
    auto &visibility = change.in_world ? out.visible_objects : out.hidden_objects;
    auto &old_visibility = change.in_world ? out.hidden_objects : out.visible_objects;
    std::erase_if(old_visibility, [&](Handle h) {
      return std::find(closure.begin(), closure.end(), h) != closure.end();
    });
    visibility.insert(visibility.end(), closure.begin(), closure.end());
  }
  changed_relations(out.effects, changes, terms, {});
  for (const auto &change : changes) {
    out.changed_objects.push_back(change.from);
    out.changed_objects.push_back(change.to);
  }
}

void change_custody(space::Space &space, IdentityRead identity, TransformWrite transforms,
                    ParentWrite parents, SpaceFlags flags, space::PhysicalRelations &physical,
                    const lifecycle::Parts &parts, std::array<TermId, 4> terms,
                    PlotWrite membership, const perm::Plots &plots, IlluminationSources &sources,
                    observe::Lights &lights, observe::ZoneAmbients &zones,
                    observe::LightSubscriptions &points, observe::ActiveViews &views,
                    NativeCompletion &out) {
  const auto custody = std::move(out.custody);
  out.custody.clear();
  std::vector<Handle> hiding;
  for (const auto &change : custody)
    if (!change.in_world) {
      const auto closure = lifecycle::collect(parts, identity, change.goods);
      hiding.insert(hiding.end(), closure.begin(), closure.end());
    }
  unique(hiding);
  // Zone bounds must dirty their old observers before custody removes the sensor.
  retire_illumination(sources, lights, zones, points, views, space, hiding);
  apply_custody(space, identity, transforms, parents, flags, physical, parts, terms, membership,
                plots, custody, out);
}

void project_visibility(IlluminationSources &sources, observe::Lights &lights,
                        observe::ZoneAmbients &zones, observe::LightSubscriptions &points,
                        observe::ActiveViews &views, net::ReplicationIndex &replication,
                        const space::Space &space, TransformRead transforms,
                        const catalog::Models &models, const catalog::ParameterBlocks &parameters,
                        const catalog::Assets &assets, const react::ValueRead &values,
                        const NativeCompletion &out, float source_radius_m, Step now, double hz) {
  retire_illumination(sources, lights, zones, points, views, space, out.hidden_objects);
  for (auto object : out.hidden_objects)
    net::retire_replication(replication, object);
  register_illumination(sources, lights, zones, points, views, space, transforms, models,
                        parameters, assets, values, out.visible_objects, source_radius_m, now, hz);
  for (auto object : out.visible_objects)
    net::register_replication(replication, object, values.identity, transforms, models, parameters,
                              assets, values);
}

void retire_commerce(perm::Ledger &ledger, perm::Offers &offers, perm::Trades &trades,
                     perm::Grants &grants, perm::Players &players, perm::OwnerLimits &owner_limits,
                     IdentityRead identity, AuthorityRead authority, PermissionFlags flags,
                     std::span<const Handle> removed, Step now, NativeCompletion &out) {
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
  if (!perm::revoke_objects(grants, players, removed).empty())
    out.permissions_changed = true;
  for (auto object : removed) {
    const auto owner = authority.owner[object.slot()];
    if (owner < players.rows.size()) {
      std::erase(players.rows[owner].inventory, object);
      std::erase(players.rows[owner].mailbox, object);
    }
    owner_limits.erase(object.slot());
  }
}

void retire_physics(space::Space &space, IdentityRead identity, ParentWrite parents,
                    SpaceFlags flags, space::PhysicalRelations &physical, Intents &intents,
                    std::vector<IntentEnding> &intent_endings, std::span<const Handle> removed,
                    std::array<TermId, 4> terms, const record::Origin &origin,
                    NativeCompletion &out) {
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

void retire_publication(net::ReplicationIndex &replication, net::SessionViews &views,
                        catalog::InstanceTexts &texts, std::span<const Handle> removed) {
  for (auto object : removed) {
    net::retire_replication(replication, object);
    texts.rows.erase(object.bits);
  }
  net::invalidate(views, removed);
}

void refresh_object_plots(const space::Space &space, IdentityRead identity,
                          TransformRead transforms, PlotWrite membership, const perm::Plots &plots,
                          const react::Terms &terms, const react::Subscriptions &subscriptions,
                          const record::Origin &origin, NativeCompletion &out) {
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
    perm::update_plots(identity, transforms, membership, plots, std::span(&object, 1), visible);
    auto dirty = react::watch_sources(subscriptions, object);
    std::erase_if(dirty, [&](FactKey fact) {
      return terms.rows.at(fact.term).source != react::Source::Setting;
    });
    // Priority depends on the contributor's current plot even without a setting watch.
    for (const auto &term : terms.rows)
      if (term.combine == react::Combine::SharePriority && term.priority)
        dirty.push_back({object, term.priority});
    std::sort(dirty.begin(), dirty.end(), [](FactKey a, FactKey b) {
      return std::pair{a.subject, a.term} < std::pair{b.subject, b.term};
    });
    dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
    for (auto source : dirty)
      out.effects.ordered.emplace_back(WatchWork{source, origin});
  }
}

void publish_catalog_changes(net::Sessions &sessions, net::SessionViews &views,
                             const catalog::Models &models, const catalog::ModelTexts &texts,
                             const react::Terms &terms, const react::Materials &materials,
                             net::MaterialAppearanceRoles appearance, NativeCompletion &out) {
  unique(out.changed_models);
  unique(out.changed_scripts);
  unique(out.changed_terms);
  unique(out.changed_materials);
  if (!out.changed_models.empty())
    sessions.broadcast(
        {{"t", "catalog"}, {"models", net::model_metadata(models, texts, out.changed_models)}});
  if (!out.changed_terms.empty())
    sessions.broadcast({{"t", "catalog"}, {"terms", net::term_metadata(terms, out.changed_terms)}});
  if (!out.changed_materials.empty())
    sessions.broadcast({{"t", "catalog"},
                        {"materials", net::material_metadata(terms, materials, appearance,
                                                             out.changed_materials)}});
  if (out.permissions_changed || !out.changed_models.empty() || !out.changed_scripts.empty() ||
      !out.changed_terms.empty() || !out.changed_materials.empty())
    for (auto &[session, view] : views.rows) {
      (void)session;
      view.pending.ready = false;
    }
}

void publish_object_changes(net::Sessions &sessions, net::SessionViews &views,
                            IdentityRead identity, AuthorityRead authority, ParentRead parents,
                            const catalog::ModelTexts &model_text,
                            const catalog::InstanceTexts &instance_text,
                            std::span<const Handle> changed) {
  std::vector<Handle> objects(changed.begin(), changed.end());
  unique(objects);
  std::erase_if(objects, [&](Handle object) { return !identity.valid(object); });
  if (objects.empty())
    return;
  net::invalidate(views, objects);
  sessions.broadcast({{"t", "metadata"},
                      {"objects", net::object_metadata(identity, authority, parents, model_text,
                                                       instance_text, objects)}});
}

} // namespace amber::step
