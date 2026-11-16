#include "editing_private.hpp"
#include "start_private.hpp"

namespace amber::step::start {
void install_profiles(const Json &plan, const Symbols &symbols, net::Profiles &profiles,
                      perm::Players &players, const std::filesystem::path &key_root) {
  for (const auto &row : plan.at("profiles")) {
    net::Profile profile;
    profile.id = row.at("id").get<PlayerId>();
    profile.name = row.at("name").get<std::string>();
    profile.roles = row.at("roles").get<std::vector<std::string>>();
    profile.connection_modes = 0;
    for (const auto &value : row.at("modes")) {
      const auto mode = value.get<std::string>();
      if (mode == "play")
        profile.connection_modes |= net::modes(net::Mode::Play);
      else if (mode == "watch")
        profile.connection_modes |= net::modes(net::Mode::Watch);
      else if (mode == "debug")
        profile.connection_modes |= net::modes(net::Mode::Debug);
      else
        throw std::invalid_argument("unknown initial connection mode");
    }
    if (row.contains("body"))
      profile.body = symbols.objects.at(row.at("body").get<std::string>());
    const auto path = key_root / row.at("key_file").get<std::string>();
    std::filesystem::create_directories(path.parent_path());
    profile.key_hashes.push_back(std::filesystem::exists(path) ? net::hash_key_file(path)
                                                               : net::create_key_file(path));
    players.rows.resize(std::max(players.rows.size(), std::size_t(profile.id) + 1));
    net::put_profile(profiles, std::move(profile));
  }
}
void install_land(const Json &plan, perm::Plots &plots, perm::Groups &groups,
                  perm::Players &players) {
  for (const auto &row : plan.at("groups")) {
    const auto id = row.at("id").get<GroupId>();
    groups.rows.resize(std::max(groups.rows.size(), std::size_t(id) + 1));
    groups.rows[id] = {id, row.at("controller").get<PlayerId>(), row.at("name").get<std::string>()};
    const auto members = row.value("members", Json::array());
    for (const auto &value : members) {
      const auto player = value.get<PlayerId>();
      players.rows.resize(std::max(players.rows.size(), std::size_t(player) + 1));
      players.rows[player].groups.push_back(id);
    }
  }
  for (const auto &row : plan.at("plots")) {
    const auto id = row.at("id").get<PlotId>();
    plots.rows.resize(std::max(plots.rows.size(), std::size_t(id) + 1));
    perm::Rights preset = 0;
    const auto &access = row.at("access");
    if (access.value("build", false))
      preset |= 1;
    if (access.value("enter", false))
      preset |= 2;
    if (access.value("run_scripts", false))
      preset |= 4;
    plots.rows[id] = {id,
                      row.at("owner").get<PlayerId>(),
                      row.value("group", GroupId{}),
                      preset,
                      row.at("bounds").get<std::array<float, 4>>(),
                      {}};
  }
}
void install_money(const Json &plan, const Symbols &symbols, IdentityRead identity,
                   AuthorityRead authority, const perm::Players &players,
                   const perm::Groups &groups, perm::Ledger &ledger) {
  std::set<perm::Account> initialized;
  for (const auto &row : plan.at("balances")) {
    auto reference = row.at("holder");
    if (const auto found = symbols.objects.find(reference.get<std::string>());
        found != symbols.objects.end())
      reference = to_string(found->second);
    const auto account = step::account(reference, identity, authority, players, groups).account;
    if (!row.at("amount").is_number_integer())
      throw std::invalid_argument("initial balance must be whole units");
    const auto amount = row.at("amount").get<std::int64_t>();
    if (amount < 0 || !initialized.insert(account).second)
      throw std::invalid_argument("invalid or duplicate initial balance");
    ledger.balance[account] = amount;
  }
}
void install_grants(const Json &plan, const Symbols &symbols, const react::ValueRead &values,
                    AuthorityRead authority, amber::perm::State &perm_state,
                    const amber::catalog::State &catalog_state,
                    const amber::react::State &react_state, const net::Profiles &profiles,
                    const amber::step::Clock &clock) {
  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &limits = perm_state.owner_limits;
  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;
  auto &instances = react_state.installations;
  auto &rules = react_state.rules;
  auto &plots = perm_state.plots;
  auto &grants = perm_state.grants;
  auto &players = perm_state.players;
  auto &groups = perm_state.groups;

  const auto holder = [&](const Json &row) -> perm::HolderControl {
    if (row.is_string()) {
      const auto id = detail::account(row);
      if (id >= players.rows.size())
        throw std::invalid_argument("unknown initial player grant holder");
      return {perm::Holder::player(id), id, true};
    }
    const auto object = symbols.objects.at(row.at("on").get<std::string>());
    const auto script = symbols.scripts.at(row.at("script").get<std::string>());
    if (!values.identity.valid(object) || !script || script >= scripts.rows.size())
      throw std::invalid_argument("unknown initial script grant holder");
    auto installed = react::installed_scripts(models, instances, values.identity, object);
    const auto applicable = react::rule_scripts(rules, values, object, now, hz);
    installed.insert(installed.end(), applicable.begin(), applicable.end());
    const bool exists = std::find(installed.begin(), installed.end(), script) != installed.end();
    return {perm::Holder::script(script, object), authority.owner[object.slot()], exists};
  };
  for (const auto &row : plan.at("grants")) {
    const auto recipient = holder(row.at("holder"));
    const auto issuer = row.at("issuer").get<PlayerId>();
    const auto caller = row.contains("caller")
                            ? holder(row.at("caller"))
                            : perm::HolderControl{perm::Holder::player(issuer), issuer, true};
    if (!caller.installed || caller.controller != issuer)
      throw std::invalid_argument("initial grant caller must be current and controlled by issuer");
    auto scope = row.at("scope");
    for (const char *field : {"object", "avatar"})
      if (scope.contains(field))
        scope[field] = to_string(symbols.objects.at(scope.at(field).get<std::string>()));
    if (scope.contains("owned_on_plot") && scope["owned_on_plot"]["owner"].is_number_integer())
      scope["owned_on_plot"]["owner"] =
          "p:" + std::to_string(scope["owned_on_plot"]["owner"].get<PlayerId>());
    if (scope.contains("account"))
      if (const auto found = symbols.objects.find(scope.at("account").get<std::string>());
          found != symbols.objects.end())
        scope["account"] = to_string(found->second);
    const bool architect = caller.holder.kind == perm::HolderKind::Player &&
                           issuer < profiles.rows.size() &&
                           std::ranges::find(profiles.rows[issuer].roles, "architect") !=
                               profiles.rows[issuer].roles.end();
    const auto control =
        step::scope(scope, values.identity, authority, limits, players, plots, groups, architect);
    const auto duration = detail::steps(row.value("expires_s", 0.), hz);
    const perm::GrantRequest request{issuer,
                                     recipient.holder,
                                     control.scope,
                                     step::rights(row.at("rights"), control.scope.kind),
                                     row.value("cap", std::int64_t{}),
                                     detail::steps(row.value("period_s", 0.), hz),
                                     duration ? now + duration : 0};
    const auto created =
        perm::create_grant(grants, players, caller.holder, request, control, recipient, now);
    if (!created)
      throw std::invalid_argument("initial grant denied by ordinary permissions: " +
                                  std::string(perm::reason_text(created.reason)));
  }
}
void provision_body_grants(const Json &plan, const Symbols &symbols,
                           const amber::catalog::State &catalog_state,
                           const amber::ObjectState &objects_state, amber::perm::State &perm_state,
                           const net::Profiles &profiles, Step now, double hz) {
  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto authority = amber::authority(std::as_const(objects_state.objects));

  auto &players = perm_state.players;
  auto &grants = perm_state.grants;

  for (const auto &row : plan.at("profiles")) {
    if (!row.contains("body") ||
        std::find(row.at("modes").begin(), row.at("modes").end(), "play") == row.at("modes").end())
      continue;
    const auto player = row.at("id").get<PlayerId>();
    const auto body = symbols.objects.at(row.at("body").get<std::string>());
    if (!identity.valid(body) || !(identity.flags[body.slot()] & Body) ||
        authority.owner[body.slot()] != player || profiles.rows.at(player).body != body)
      throw std::invalid_argument("starting body must be the configured player's own Body");
    const auto script = models.rows.at(identity.model[body.slot()]).script;
    if (!script)
      continue;
    const auto &source = scripts.rows.at(script);
    if (source.declaration.slots.empty())
      continue;
    if (!row.contains("body_installs") || !row.at("body_installs").is_array())
      throw std::invalid_argument("starting body needs authored body_installs for its slots");
    NativeCall call;
    call.request.caller = player;
    call.request.target = body;
    call.body = body;
    call.step = now;
    call.hz = hz;
    std::set<std::string> filled;
    for (auto choices : row.at("body_installs")) {
      for (auto &choice : choices) {
        auto &scope = choice.at("scope");
        for (const char *field : {"object", "avatar"})
          if (scope.contains(field))
            scope[field] = to_string(symbols.objects.at(scope.at(field).get<std::string>()));
        if (scope.contains("owned_on_plot") && scope["owned_on_plot"]["owner"].is_number_integer())
          scope["owned_on_plot"]["owner"] =
              "p:" + std::to_string(scope["owned_on_plot"]["owner"].get<PlayerId>());
      }
      const auto requested = editing::detail::slot_choices(
          choices, source, objects_state, perm_state, profiles.rows[player].roles, call);
      const auto granted =
          editing::detail::fill_slots(source, requested, authority, players, grants, call);
      if (granted.reply.at("grants").size() != choices.size())
        throw std::invalid_argument("starting body slot grant denied: " +
                                    granted.reply.at("unfilled_slots").dump());
      for (const auto &choice : choices)
        filled.insert(choice.at("slot").get<std::string>());
    }
    for (const auto &slot : source.declaration.slots)
      if (!filled.contains(slot))
        throw std::invalid_argument("starting body has no authored choice for slot: " + slot);
  }
}
} // namespace amber::step::start
