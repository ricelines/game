#include "step.hpp"

#include <algorithm>
#include <tuple>

namespace amber::step {
const amber::Origin &move_origin(const Intents &intents, Handle body) {
  static const amber::Origin empty;
  const auto found = intents.moving.find(body.bits);
  return found == intents.moving.end() ? empty : found->second.origin;
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
const amber::Origin &interaction_origin(const Intents &intents, Handle body, Handle other) {
  const auto found = intents.moving.find(body.bits);
  return found == intents.moving.end() ? move_origin(intents, other) : found->second.origin;
}
} // namespace
void relation_effects(react::DeferredEffects &effects, const PhysicalRelationTerms &terms,
                      std::span<const space::RelationChange> changes) {
  for (const auto &change : changes) {
    const auto term = relation_term(terms, change.kind);
    effects.ordered.emplace_back(react::WatchWork{{change.from, term}, {}});
    effects.ordered.emplace_back(react::WatchWork{{change.to, term}, {}});
  }
}
void publish_physical(amber::step::State &step_state, record::Journal &journal,
                      const PhysicalEventTerms &terms, std::size_t event_limit,
                      const space::StepResult &physics, const amber::step::Clock &clock) {
  const auto now = clock.step;
  const auto &intents = step_state.intents;
  auto &queues = step_state.queues;

  for (const auto &contact : physics.contacts) {
    for (const auto &[receiver, sender, normal] :
         {std::tuple{contact.first, contact.second, contact.normal},
          std::tuple{contact.second, contact.first, contact.normal * -1}}) {
      const auto &origin = interaction_origin(intents, receiver, sender);
      const auto result = step::enqueue_event(
          queues,
          {receiver, sender, terms.contact,
           std::vector<EventField>{
               {"point", contact.point}, {"normal", normal}, {"started", Value(contact.started)}},
           origin},
          event_limit, now);
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
  for (const auto &change : physics.relations) {
    const auto term = relation_term(terms.relations, change.kind);
    const auto &origin = interaction_origin(intents, change.from, change.to);
    for (auto object : {change.from, change.to})
      step::enqueue_watch(queues, {{object, term}, origin});
  }
}
space::StepResult advance_motion(space::State &space_state, amber::ObjectState &objects_state,
                                 amber::step::State &step_state,
                                 const amber::perm::State &perm_state,
                                 const amber::step::Clock &clock, float goal_tolerance_m,
                                 float reach_m) {
  const auto now = clock.step;
  const auto hz = clock.hz;

  auto ids = amber::identity(std::as_const(objects_state.objects));

  auto &intents = step_state.intents;
  auto &plots = perm_state.plots;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;
  auto auth = amber::authority(std::as_const(objects_state.objects));

  std::vector<space::MoveRequest> moving;
  moving.reserve(intents.moving.size());
  for (const auto &[bits, intent] : intents.moving) {
    space::Goal goal;
    if (const auto *point = std::get_if<Vec3>(&intent.goal))
      goal = space::PointGoal{*point, goal_tolerance_m};
    else
      goal = space::ObjectGoal{std::get<Handle>(intent.goal), reach_m};
    const space::MoveRequest request{Handle{bits}, intent.id, std::move(goal), intent.speed_m_s};
    if (space::needs_plan(space_state, ids, request)) {
      // Only plots that the candidate routes cross can close them.
      const auto candidates = space::routes(space_state, ids, request.body, request.goal);
      const auto &e = candidates.extent;
      std::vector<space::TravelBoundary> forbidden;
      const auto player = perm::Holder::player(auth.owner[request.body.slot()]);
      for (const auto id : perm::plots_touching(plots, {e.min.x, e.min.y, e.max.x, e.max.y}))
        if (const auto &plot = plots.rows[id];
            !perm::check_land(player, perm::LandVerb::Enter, plot, players, grants, now)) {
          const auto [x0, y0, x1, y1] = plot.bounds;
          forbidden.push_back({{{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}},
                               std::numeric_limits<float>::lowest(),
                               std::numeric_limits<float>::max()});
        }
      space::plan_move(space_state, ids, request, candidates, forbidden);
    }
    moving.push_back(request);
  }
  space::StepResult physics;
  space::step(space_state.space, objects_state, space_state.physical_relations, now, 1.0f / hz,
              moving, physics);
  return physics;
}
void finish_motion(State &state, std::span<const space::IntentProgress> progress) {
  for (const auto &item : progress)
    if (item.state == space::IntentState::Arrived || item.state == space::IntentState::Blocked ||
        item.state == space::IntentState::TargetGone) {
      if (auto ending = step::finish_move(state.intents, item.body,
                                          item.state == space::IntentState::Arrived
                                              ? step::IntentEnd::Arrived
                                              : step::IntentEnd::Blocked))
        state.intent_endings.push_back(std::move(*ending));
    }
}
void publish_intent_endings(amber::step::State &step_state, const react::Terms &terms,
                            TermId arrived, TermId blocked, TermId cancelled,
                            const amber::ObjectState &objects_state, net::Sessions &sessions,
                            record::Journal &journal, Step now, std::size_t event_queue_max) {
  auto &intent_endings = step_state.intent_endings;
  auto ids = amber::identity(std::as_const(objects_state.objects));
  auto auth = amber::authority(std::as_const(objects_state.objects));
  auto &queues = step_state.queues;

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
        event_queue_max, now);
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
  // Sorted once per step so each look finds a candidate's speed by search.
  std::ranges::sort(motion, {}, [](const observe::MotionSample &row) { return row.object.bits; });
}
} // namespace amber::step
