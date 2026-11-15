#include "private.hpp"

namespace amber::bindings::commerce {
std::size_t deliver_mailbox(MailboxDelivery &b, PlayerId player, Handle body, Step now,
                            const record::Origin &origin, react::DeferredEffects &effects) {
  const auto identity = b.custody.identity;
  if (!player || player >= b.players.rows.size() || !identity.valid(body) ||
      !(identity.flags[body.slot()] & Body) || b.authority.owner[body.slot()] != player)
    throw std::invalid_argument("mailbox delivery requires the authenticated player's live body");
  const auto &mailbox = b.players.rows[player].mailbox;
  if (mailbox.empty())
    return 0;
  std::vector<Handle> things{body};
  for (auto root : mailbox) {
    if (!identity.valid(root) || b.authority.owner[root.slot()] != player)
      throw std::invalid_argument("mailbox goods are no longer owned by the recipient");
    detail::preflight(b.custody, {root, body, false});
    const auto closure = lifecycle::collect(b.custody.parts, identity, root);
    things.insert(things.end(), closure.begin(), closure.end());
  }
  const auto count = mailbox.size();
  const auto changes = perm::deliver_mailbox(b.players, player, body);
  const auto previous = effects.ordered.size();
  detail::apply(b.custody, changes, effects);
  for (auto i = previous; i < effects.ordered.size(); ++i)
    if (auto *work = std::get_if<step::WatchWork>(&effects.ordered[i]))
      work->origin = origin;
  record::Event event;
  event.step = now;
  event.kind = record::Kind::System;
  event.actor = record::Actor::player(player);
  event.name = "mailbox_delivery";
  event.changed = true;
  event.result = record::Result::Applied;
  event.things = std::move(things);
  event.detail = {{"delivered", count}, {"body", to_string(body)}};
  effects.ordered.emplace_back(std::move(event));
  effects.changed = true;
  return count;
}
} // namespace amber::bindings::commerce
