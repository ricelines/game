#include "commerce_private.hpp"

namespace amber::step {
NativeCompletion script_notice(const perm::State &state, const NativeCall &call) {
  const auto recipient = commerce_detail::player_id(call.arguments.at("player"), state.players);
  const auto caller = commerce_detail::actor(call);
  bool allowed = false;
  if (caller.kind == perm::HolderKind::Script)
    for (auto id : state.players.rows[recipient].grants_given) {
      const auto &grant = state.grants.rows[id];
      if (grant.live && (!grant.expires_step || grant.expires_step > call.step) &&
          grant.holder == caller) {
        allowed = true;
        break;
      }
    }
  if (!allowed)
    return commerce_detail::denied(
        "notice", "p:" + std::to_string(recipient),
        "only a script holding this player's live grant may send a notice",
        "ask the player to give this script a grant first");
  auto text = call.arguments.at("text").get<std::string>();
  std::size_t end = 0, points = 0;
  while (end < text.size()) {
    if ((static_cast<unsigned char>(text[end]) & 0xc0) != 0x80 && points++ == 280)
      break;
    ++end;
  }
  text.resize(end);
  auto out = commerce_detail::result(true);
  out.notices.emplace_back(recipient, Json{{"kind", "notice"},
                                           {"from", "s:" + std::to_string(caller.id)},
                                           {"on", to_string(caller.installed_on)},
                                           {"text", text}});
  commerce_detail::record(out, call, "notify", {caller.installed_on},
                          {{"player", recipient}, {"text", text}});
  return out;
}

void notify(perm::Players &players, net::Sessions &sessions, std::size_t mailbox_max,
            PlayerId player, Json notice) {
  if (!player)
    return;
  for (auto id : sessions.sessions_for(player))
    if (sessions.principal(id)->mode == net::Mode::Play) {
      sessions.event_for_player(player, notice);
      return;
    }
  auto &row = players.rows.at(player);
  row.notices.push_back(std::move(notice));
  if (row.notices.size() > mailbox_max) {
    row.notices.erase(row.notices.begin());
    ++row.notices_dropped;
  }
}
step::NativeCompletion deliver_mailbox(perm::Players &players, net::Sessions &sessions,
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
  auto &row = players.rows[player];
  for (std::size_t i = 0; i < row.notices.size(); ++i) {
    if (i == 0 && row.notices_dropped)
      row.notices[i]["dropped"] = row.notices_dropped;
    sessions.event_for_player(player, row.notices[i]);
  }
  row.notices.clear();
  row.notices_dropped = 0;
  const auto &mailbox = row.mailbox;
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
