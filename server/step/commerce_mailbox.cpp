#include "commerce_private.hpp"

namespace amber::step {
step::NativeCompletion deliver_mailbox(perm::Players &players,
                                       const amber::ObjectState &objects_state,
                                       const space::Space &space, PlayerId player, Handle body,
                                       Step now) {
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto parents = amber::parents(std::as_const(objects_state.objects));
  auto &parts = objects_state.parts;

  if (!player || player >= players.rows.size() || !identity.valid(body) ||
      !(identity.flags[body.slot()] & Body) || authority.owner[body.slot()] != player)
    throw std::invalid_argument("mailbox delivery requires the authenticated player's live body");
  const auto &mailbox = players.rows[player].mailbox;
  if (mailbox.empty())
    return commerce_detail::result(false, {{"delivered", 0}});
  std::vector<Handle> things{body};
  for (auto root : mailbox) {
    if (!identity.valid(root) || authority.owner[root.slot()] != player)
      throw std::invalid_argument("mailbox goods are no longer owned by the recipient");
    commerce_detail::check_custody(space, identity, parents, parts, {root, body, false});
    const auto closure = lifecycle::collect(parts, identity, root);
    things.insert(things.end(), closure.begin(), closure.end());
  }
  const auto count = mailbox.size();
  const auto changes = perm::deliver_mailbox(players, player, body);
  auto out = commerce_detail::result(true, {{"delivered", count}});
  out.custody = changes;
  out.changed_objects = things;
  record::Event event;
  event.step = now;
  event.kind = record::Kind::System;
  event.actor = record::Actor::player(player);
  event.name = "mailbox_delivery";
  event.changed = true;
  event.result = record::Result::Applied;
  event.things = std::move(things);
  event.detail = {{"delivered", count}, {"body", to_string(body)}, {"agent", false}};
  out.records.push_back(std::move(event));
  return out;
}
} // namespace amber::step
