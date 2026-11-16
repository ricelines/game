#include "perm.hpp"
#include "snap/snap.hpp"
#include <algorithm>

namespace amber::perm {
SnapshotSelection capture_authority(AuthorityRead authority, std::span<const Handle> objects,
                                    std::span<const PlayerId> actors,
                                    const amber::perm::State &perm_state) {
  auto &players = perm_state.players;
  auto &plots = perm_state.plots;
  auto &groups = perm_state.groups;
  auto &grants = perm_state.grants;
  auto &offers = perm_state.offers;
  auto &trades = perm_state.trades;

  SnapshotSelection selected;
  selected.objects = snap::ObjectSelection(objects);
  selected.players.insert(actors.begin(), actors.end());
  for (auto object : objects) {
    selected.players.insert(authority.owner[object.slot()]);
    selected.groups.insert(authority.group[object.slot()]);
    selected.plots.insert(authority.plot[object.slot()]);
  }
  for (const auto &offer : offers.rows)
    if (offer.id &&
        (offer.plot ? selected.plots.contains(offer.plot)
                    : selected.objects.contains(offer.goods)) &&
        std::all_of(offer.parts.begin(), offer.parts.end(),
                    [&](Handle part) { return selected.objects.contains(part); })) {
      if (offer.maker.kind == HolderKind::Script) {
        selected.objects.slots.insert(offer.maker.installed_on.slot());
        selected.objects.objects.insert(offer.maker.installed_on.bits);
        const auto slot = offer.maker.installed_on.slot();
        selected.players.insert(authority.owner[slot]);
        selected.groups.insert(authority.group[slot]);
        selected.plots.insert(authority.plot[slot]);
      } else
        selected.players.insert(offer.maker.id);
      selected.offers.insert(offer.id);
      selected.players.insert(offer.seller);
      selected.players.insert(offer.buyer);
    }
  for (auto id : selected.plots)
    if (id && id < plots.rows.size()) {
      selected.players.insert(plots.rows[id].owner);
      selected.groups.insert(plots.rows[id].group);
    }
  const auto initial_players = selected.players;
  for (auto id : initial_players)
    if (id && id < players.rows.size())
      selected.groups.insert(players.rows[id].groups.begin(), players.rows[id].groups.end());
  for (auto id : selected.groups)
    if (id && id < groups.rows.size())
      selected.players.insert(groups.rows[id].controller);
  auto scope_selected = [&](Scope scope) {
    switch (scope.kind) {
    case ScopeKind::Object:
    case ScopeKind::Avatar:
      return selected.objects.contains(Handle{scope.subject});
    case ScopeKind::Owned:
      return selected.players.contains(scope.owner);
    case ScopeKind::AllLand:
      return true;
    case ScopeKind::OwnedOnPlot:
      return selected.players.contains(scope.owner) && selected.plots.contains(scope.subject);
    case ScopeKind::Plot:
      return selected.plots.contains(scope.subject);
    case ScopeKind::Account:
      switch (scope.account_kind) {
      case AccountKind::Player:
        return selected.players.contains(static_cast<PlayerId>(scope.subject));
      case AccountKind::Object:
        return selected.objects.contains(Handle{scope.subject});
      case AccountKind::Group:
        return selected.groups.contains(static_cast<GroupId>(scope.subject));
      }
      return false;
    }
    return false;
  };
  std::vector<GrantId> pending;
  for (const auto &grant : grants.rows) {
    const bool holder = grant.holder.kind == HolderKind::Player
                            ? selected.players.contains(grant.holder.id)
                            : selected.objects.contains(grant.holder.installed_on);
    if (grant.id && holder && scope_selected(grant.scope))
      pending.push_back(grant.id);
  }
  while (!pending.empty()) {
    auto id = pending.back();
    pending.pop_back();
    if (!selected.grants.insert(id).second)
      continue;
    const auto &grant = grants.rows.at(id);
    selected.players.insert(grant.issuer);
    if (grant.holder.kind == HolderKind::Player)
      selected.players.insert(grant.holder.id);
    if (grant.parent)
      pending.push_back(grant.parent);
  }
  for (const auto &trade : trades.rows)
    if (trade.id && selected.players.contains(trade.players[0]) &&
        selected.players.contains(trade.players[1]) &&
        std::all_of(trade.sides.begin(), trade.sides.end(), [&](const auto &side) {
          return !side.offer || selected.offers.contains(side.offer);
        }))
      selected.trades.insert(trade.id);
  selected.players.erase(0);
  selected.groups.erase(0);
  selected.plots.erase(0);
  return selected;
}
template <class S> void serialize(S &s, Holder &v) {
  s.value1b(v.kind);
  s.value4b(v.id);
  s.object(v.installed_on);
}
template <class S> void serialize(S &s, Scope &v) {
  s.value1b(v.kind);
  s.value8b(v.subject);
  s.value4b(v.owner);
  s.value1b(v.account_kind);
}
template <class S> void serialize(S &s, Account &v) {
  s.value1b(v.kind);
  s.value8b(v.subject);
}
template <class S> void serialize(S &s, Player &v) {
  snap::field(s, v.groups);
  snap::field(s, v.grants_held);
  snap::field(s, v.grants_given);
  snap::field(s, v.inventory);
  snap::field(s, v.mailbox);
  s.container(v.notices, snap::max_collection(), [](S &stream, nlohmann::json &notice) {
    std::string text;
    if constexpr (std::is_same_v<S, snap::Output>)
      text = notice.dump();
    snap::field(stream, text);
    if constexpr (std::is_same_v<S, snap::Input>)
      notice = nlohmann::json::parse(text);
  });
  s.value4b(v.notices_dropped);
}
template <class S> void serialize(S &s, Group &v) {
  s.value4b(v.id);
  s.value4b(v.controller);
  snap::field(s, v.name);
}
template <class S> void serialize(S &s, Plot &v) {
  s.value4b(v.id);
  s.value4b(v.owner);
  s.value4b(v.group);
  s.value4b(v.preset);
  s.value1b(v.kind);
  s.container4b(v.bounds);
  snap::field(s, v.banned);
}
template <class S> void serialize(S &s, Grant &v) {
  s.value4b(v.id);
  s.value4b(v.issuer);
  s.value4b(v.parent);
  s.object(v.holder);
  s.object(v.scope);
  s.value4b(v.verbs);
  s.value8b(v.cap);
  s.value8b(v.spent);
  s.value8b(v.period_steps);
  s.value8b(v.period_start);
  s.value8b(v.expires_step);
  s.boolValue(v.live);
}
template <class S> void serialize(S &s, Offer &v) {
  s.value4b(v.id);
  s.value4b(v.seller);
  s.value4b(v.buyer);
  s.object(v.goods);
  s.value8b(v.price);
  s.value8b(v.expires_step);
  s.value4b(v.trade);
  s.boolValue(v.live);
  snap::field(s, v.parts);
  s.value4b(v.plot);
  s.object(v.maker);
}
template <class S> void serialize(S &s, CustodyChange &v) {
  s.object(v.goods);
  s.object(v.parent);
  s.boolValue(v.in_world);
}
template <class S> void serialize(S &s, TradeSide &v) {
  s.value4b(v.offer);
  s.value8b(v.money);
  s.boolValue(v.confirmed);
}
template <class S> void serialize(S &s, Trade &v) {
  s.value4b(v.id);
  s.container4b(v.players);
  s.container(v.sides);
  s.boolValue(v.live);
}
void write_players(snap::Output &out, const Players &players, const Ledger &ledger,
                   const Plots &plots, const Groups &groups, const SnapshotSelection *selected) {
  auto player_rows = selected ? players.rows : std::vector<Player>{};
  if (selected)
    for (std::size_t id = 1; id < player_rows.size(); ++id) {
      auto &row = player_rows[id];
      if (!selected->players.contains(id)) {
        row = Player{};
        continue;
      }
      std::erase_if(row.inventory, [&](Handle h) { return !selected->objects.contains(h); });
      std::erase_if(row.mailbox, [&](Handle h) { return !selected->objects.contains(h); });
      std::erase_if(row.grants_given, [&](GrantId g) { return !selected->grants.contains(g); });
      std::erase_if(row.grants_held, [&](GrantId g) { return !selected->grants.contains(g); });
    }
  out.container(selected ? player_rows : players.rows, snap::max_collection());
  auto included = [&](Account) { return !selected; };
  auto count = static_cast<std::uint32_t>(
      std::count_if(ledger.balance.begin(), ledger.balance.end(),
                    [&](const auto &row) { return included(row.first); }));
  out.value4b(count);
  for (const auto &[key, amount] : ledger.balance)
    if (included(key)) {
      auto account = key;
      auto value = amount;
      out.object(account);
      out.value8b(value);
    }
  auto plot_rows = selected ? plots.rows : std::vector<Plot>{};
  auto group_rows = selected ? groups.rows : std::vector<Group>{};
  if (selected) {
    for (auto &row : plot_rows)
      if (!selected->plots.contains(row.id))
        row = Plot{};
    for (auto &row : group_rows)
      if (!selected->groups.contains(row.id))
        row = Group{};
  }
  out.container(selected ? plot_rows : plots.rows, snap::max_collection());
  out.container(selected ? group_rows : groups.rows, snap::max_collection());
}
void read_players(snap::Input &in, Players &players, Ledger &ledger, Plots &plots, Groups &groups) {
  snap::field(in, players.rows);
  std::uint32_t count = 0;
  in.value4b(count);
  ledger.balance.clear();
  while (count--) {
    Account account;
    std::int64_t value = 0;
    in.object(account);
    in.value8b(value);
    if (!account.subject || account.kind > AccountKind::Group || value < 0 ||
        !ledger.balance.emplace(account, value).second)
      throw std::invalid_argument("invalid or duplicate account balance");
  }
  snap::field(in, plots.rows);
  snap::field(in, groups.rows);
}
void write_grants(snap::Output &out, const Grants &grants, const Offers &offers,
                  const Trades &trades, const OwnerLimits &limits,
                  const SnapshotSelection *selected) {
  auto grant_rows = selected ? grants.rows : std::vector<Grant>{};
  auto offer_rows = selected ? offers.rows : std::vector<Offer>{};
  auto trade_rows = selected ? trades.rows : std::vector<Trade>{};
  if (selected) {
    for (auto &row : grant_rows)
      if (!selected->grants.contains(row.id))
        row = Grant{};
    for (auto &row : offer_rows)
      if (!selected->offers.contains(row.id))
        row = Offer{};
    for (auto &row : trade_rows)
      if (!selected->trades.contains(row.id)) {
        row = Trade{};
        row.live = false;
      }
  }
  out.container(selected ? grant_rows : grants.rows, snap::max_collection());
  out.container(selected ? offer_rows : offers.rows, snap::max_collection());
  out.container(selected ? trade_rows : trades.rows, snap::max_collection());
  auto field = [](snap::Output &s, Rights &value) { s.value4b(value); };
  if (!selected) {
    snap::write_sparse(out, limits, field);
    return;
  }
  OwnerLimits kept;
  for (auto slot : selected->objects.slots)
    if (auto row = limits.get(slot))
      kept.set(slot, *row);
  snap::write_sparse(out, kept, field);
}
void read_grants(snap::Input &in, Grants &grants, Offers &offers, Trades &trades,
                 OwnerLimits &limits, std::size_t slots) {
  snap::field(in, grants.rows);
  snap::field(in, offers.rows);
  snap::field(in, trades.rows);
  snap::read_sparse(in, limits, [](snap::Input &s, Rights &value) { s.value4b(value); });
  limits.rebuild_positions(slots);
  rebuild_grant_lookup(grants);
  rebuild_offer_deadlines(offers);
}
} // namespace amber::perm
