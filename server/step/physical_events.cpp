#include "step.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace amber::step {
struct MechanicalCauses::Data {
  std::size_t authors_max;
  space::CauseToken next = 1;
  std::unordered_map<space::CauseToken, CauseSummary> entries;
  std::unordered_map<std::uint64_t, space::CauseToken> interned;
};
MechanicalCauses::MechanicalCauses(std::size_t authors_max) : data(std::make_unique<Data>()) {
  data->authors_max = authors_max;
}
MechanicalCauses::~MechanicalCauses() = default;
space::CauseToken MechanicalCauses::intern(const record::Origin &origin,
                                           space::CauseToken previous) {
  if (previous)
    return restore(combine(lookup(previous), {origin, false}));
  if (!origin.cause && !origin.root)
    return 0;
  if (auto found = data->interned.find(origin.cause);
      found != data->interned.end() && lookup(found->second).origin == origin)
    return found->second;
  auto token = restore({origin, false});
  data->interned[origin.cause] = token;
  return token;
}
space::CauseToken MechanicalCauses::restore(CauseSummary summary) {
  auto token = data->next++;
  data->entries.emplace(token, std::move(summary));
  return token;
}
CauseSummary MechanicalCauses::lookup(space::CauseToken token) const {
  return token ? data->entries.at(token) : CauseSummary{};
}
CauseSummary MechanicalCauses::combine(const CauseSummary &a, const CauseSummary &b) const {
  const auto empty = [](const CauseSummary &v) {
    return !v.ambiguous && !v.origin.cause && !v.origin.root;
  };
  if (empty(a))
    return b;
  if (empty(b))
    return a;
  CauseSummary result = a.origin.cause > b.origin.cause ? a : b;
  result.ambiguous = a.ambiguous || b.ambiguous || a.origin.root != b.origin.root;
  result.origin.authors = a.origin.authors;
  result.origin.authors.insert(result.origin.authors.end(), b.origin.authors.begin(),
                               b.origin.authors.end());
  auto &authors = result.origin.authors;
  std::sort(authors.begin(), authors.end());
  authors.erase(std::unique(authors.begin(), authors.end()), authors.end());
  result.origin.more = a.origin.more || b.origin.more || authors.size() > data->authors_max;
  if (authors.size() > data->authors_max)
    authors.resize(data->authors_max);
  if (a.origin.oldest && b.origin.oldest)
    result.origin.oldest = std::min(*a.origin.oldest, *b.origin.oldest);
  else
    result.origin.oldest = a.origin.oldest ? a.origin.oldest : b.origin.oldest;
  if (result.ambiguous)
    result.origin.cause = result.origin.root = 0;
  return result;
}
void MechanicalCauses::retain(std::span<const space::CauseToken> tokens) {
  std::unordered_set<space::CauseToken> live(tokens.begin(), tokens.end());
  std::erase_if(data->entries, [&](const auto &row) { return !live.contains(row.first); });
  std::erase_if(data->interned,
                [&](const auto &row) { return !data->entries.contains(row.second); });
}
namespace {
TermId relation_term(const PhysicalRelationTerms &terms, space::RelationKind kind) {
  switch (kind) {
  case space::RelationKind::Contains:
    return terms.contains;
  case space::RelationKind::Supports:
    return terms.supports;
  case space::RelationKind::AttachedTo:
    return terms.attached_to;
  case space::RelationKind::InZone:
    return terms.in_zone;
  }
  std::terminate();
}
record::Origin published_origin(record::Journal &journal, IdentityRead identity,
                                AttributionRead attribution, const catalog::Models &models,
                                const catalog::Scripts &scripts, double hz,
                                const CauseSummary &summary, Step step,
                                std::span<const Handle> things) {
  if (!summary.ambiguous) {
    auto origin = summary.origin;
    const auto credits = record::credits(identity, attribution, models, scripts, things, 0, hz);
    journal.include_credits(origin, credits);
    return origin;
  }
  record::Event event;
  event.step = step;
  event.kind = record::Kind::System;
  event.name = "physical_interaction";
  event.result = record::Result::NoEffect;
  event.changed = false;
  event.things.assign(things.begin(), things.end());
  event.detail["causal_ambiguity"] = true;
  event.credits = record::credits(identity, attribution, models, scripts, things, 0, hz);
  return journal.append(std::move(event), summary.origin);
}
} // namespace
void relation_effects(react::DeferredEffects &effects, const PhysicalRelationTerms &terms,
                      std::span<const space::RelationChange> changes) {
  for (const auto &change : changes) {
    const auto term = relation_term(terms, change.kind);
    effects.ordered.emplace_back(step::WatchWork{{change.from, term}, {}});
    effects.ordered.emplace_back(step::WatchWork{{change.to, term}, {}});
  }
}
void publish_physical(MechanicalCauses &causes, step::WorkQueues &queues, record::Journal &journal,
                      const PhysicalEventTerms &terms, std::size_t event_limit, double hz,
                      IdentityRead identity, AttributionRead attribution,
                      const catalog::Models &models, const catalog::Scripts &scripts,
                      const space::StepResult &physics, Step now) {
  for (const auto &contact : physics.contacts) {
    const auto summary =
        causes.combine(causes.lookup(contact.causes.first), causes.lookup(contact.causes.second));
    const std::array<Handle, 2> things{contact.first, contact.second};
    const auto origin =
        published_origin(journal, identity, attribution, models, scripts, hz, summary, now, things);
    for (const auto &[receiver, sender, normal] :
         {std::tuple{contact.first, contact.second, contact.normal},
          std::tuple{contact.second, contact.first, contact.normal * -1}}) {
      const auto result = step::enqueue_event(
          queues,
          {receiver, sender, terms.contact,
           std::vector<EventField>{
               {"point", contact.point}, {"normal", normal}, {"started", Value(contact.started)}},
           origin},
          event_limit);
      if (result == step::PushResult::Overflow) {
        record::Event event;
        event.step = now;
        event.kind = record::Kind::System;
        event.name = "overflow";
        event.things = {receiver, sender};
        event.detail = {
            {"queue", "events"}, {"limit", event_limit}, {"dropped_term", terms.contact}};
        journal.append(std::move(event), origin);
      }
    }
  }
  std::map<std::pair<std::uint64_t, TermId>, CauseSummary> facts;
  for (const auto &change : physics.relations) {
    const auto summary =
        causes.combine(causes.lookup(change.causes.first), causes.lookup(change.causes.second));
    const auto term = relation_term(terms.relations, change.kind);
    for (auto object : {change.from, change.to}) {
      auto &combined = facts[{object.bits, term}];
      combined = causes.combine(combined, summary);
    }
  }
  for (const auto &[key, summary] : facts) {
    const Handle object{key.first};
    const auto origin = published_origin(journal, identity, attribution, models, scripts, hz,
                                         summary, now, std::span(&object, 1));
    step::enqueue_watch(queues, {{object, key.second}, origin});
  }
}
void collect_physical_causes(MechanicalCauses &causes, const space::Space &space) {
  std::vector<space::CauseToken> tokens;
  space::active_cause_tokens(space, tokens);
  causes.retain(tokens);
}
space::StepResult advance_motion(space::Space &space, IdentityRead ids, TransformWrite poses,
                                 ParentWrite parent, SpaceFlags physical_flags,
                                 space::PhysicalRelations &physical_relations, Intents &intents,
                                 std::vector<IntentEnding> &intent_endings,
                                 TravelAccess &travel_access, const perm::Plots &plots,
                                 const perm::Players &players, const perm::Grants &grants,
                                 AuthorityRead auth, MechanicalCauses &mechanical_causes, Step now,
                                 double hz, float goal_tolerance_m, float reach_m) {
  std::vector<space::MoveRequest> moving;
  moving.reserve(intents.moving.size());
  for (const auto &[bits, intent] : intents.moving) {
    space::Goal goal;
    if (const auto *point = std::get_if<Vec3>(&intent.goal))
      goal = space::PointGoal{*point, goal_tolerance_m};
    else
      goal = space::ObjectGoal{std::get<Handle>(intent.goal), reach_m};
    moving.push_back(
        {Handle{bits}, intent.id, std::move(goal),
         travel_boundaries(travel_access, ids, auth, Handle{bits}, plots, players, grants, now),
         intent.speed_m_s, mechanical_causes.intern(intent.origin)});
  }
  std::erase_if(travel_access.bodies,
                [&](const auto &row) { return !intents.moving.contains(row.first); });
  space::StepResult physics;
  space::step(space, ids, poses, parent, physical_flags, physical_relations, now, 1.0f / hz, moving,
              physics);
  for (const auto &progress : physics.intents)
    if (progress.state == space::IntentState::Arrived ||
        progress.state == space::IntentState::Blocked ||
        progress.state == space::IntentState::TargetGone) {
      if (auto ending = step::finish_move(intents, progress.body,
                                          progress.state == space::IntentState::Arrived
                                              ? step::IntentEnd::Arrived
                                              : step::IntentEnd::Blocked))
        intent_endings.push_back(std::move(*ending));
    }
  return physics;
}
void publish_intent_endings(std::vector<IntentEnding> &intent_endings, const react::Terms &terms,
                            TermId arrived, TermId blocked, TermId cancelled, IdentityRead ids,
                            AuthorityRead auth, net::Sessions &sessions, WorkQueues &queues,
                            record::Journal &journal, Step now, std::size_t event_queue_max) {
  for (const auto &ending : intent_endings) {
    const auto event = ending.reason == step::IntentEnd::Arrived   ? arrived
                       : ending.reason == step::IntentEnd::Blocked ? blocked
                                                                   : cancelled;
    const auto queued_ending = step::enqueue_event(
        queues,
        {ending.body,
         {},
         event,
         std::vector<EventField>{{"intent", Value(static_cast<std::int64_t>(ending.intent.id))},
                                 {"reason", std::string(ending.reason == step::IntentEnd::Replaced
                                                            ? "replaced"
                                                            : terms.rows[event].name)}},
         ending.intent.origin},
        event_queue_max);
    if (queued_ending == step::PushResult::Overflow) {
      record::Event dropped;
      dropped.step = now;
      dropped.kind = record::Kind::System;
      dropped.name = "overflow";
      dropped.things = {ending.body};
      dropped.detail = {{"queue", "events"}, {"limit", event_queue_max}, {"dropped_term", event}};
      journal.append(std::move(dropped), ending.intent.origin);
    }
    if (ids.valid(ending.body))
      sessions.event_for_player(
          auth.owner[ending.body.slot()],
          {{"kind", ending.reason == step::IntentEnd::Arrived   ? "arrived"
                    : ending.reason == step::IntentEnd::Blocked ? "blocked"
                                                                : "cancelled"},
           {"body", to_string(ending.body)},
           {"intent", ending.intent.id},
           {"reason",
            ending.reason == step::IntentEnd::Replaced ? "replaced" : terms.rows[event].name}});
  }
  intent_endings.clear();
}
void sample_motion(std::vector<observe::MotionSample> &motion,
                   SparseColumn<std::pair<Handle, Vec3>> &previous_positions,
                   std::span<const Handle> moved, const space::Space &space, IdentityRead ids,
                   TransformRead poses, double hz) {
  motion.clear();
  for (auto object : moved) {
    const auto position = poses.position[object.slot()];
    const auto *previous = previous_positions.get(object.slot());
    auto velocity =
        previous && previous->first == object ? (position - previous->second) * hz : Vec3{};
    if (const auto native = space::motion(space, ids, object);
        native && native->linear_velocity_m_s.length() > velocity.length())
      velocity = native->linear_velocity_m_s;
    motion.push_back({object, velocity});
    previous_positions.set(object.slot(), std::pair{object, position});
  }
}
} // namespace amber::step
