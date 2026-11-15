#include "common.hpp"

namespace amber::bindings::start {
void install_profiles(const Json &plan, const Symbols &symbols, control::Profiles &profiles,
                      perm::Players &players, const std::filesystem::path &key_root) {
  for (const auto &row : plan.at("profiles")) {
    control::Profile profile;
    profile.id = row.at("id").get<PlayerId>();
    profile.name = row.at("name").get<std::string>();
    profile.roles = row.at("roles").get<std::vector<std::string>>();
    profile.connection_modes = 0;
    for (const auto &value : row.at("modes")) {
      const auto mode = value.get<std::string>();
      if (mode == "play")
        profile.connection_modes |= control::modes(control::Mode::Play);
      else if (mode == "watch")
        profile.connection_modes |= control::modes(control::Mode::Watch);
      else if (mode == "debug")
        profile.connection_modes |= control::modes(control::Mode::Debug);
      else
        throw std::invalid_argument("unknown initial connection mode");
    }
    if (row.contains("body"))
      profile.body = symbols.objects.at(row.at("body").get<std::string>());
    const auto path = key_root / row.at("key_file").get<std::string>();
    std::filesystem::create_directories(path.parent_path());
    profile.key_hashes.push_back(std::filesystem::exists(path) ? control::hash_key_file(path)
                                                               : control::create_key_file(path));
    players.rows.resize(std::max(players.rows.size(), std::size_t(profile.id) + 1));
    control::put_profile(profiles, std::move(profile));
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
void install_money(const Json &plan, const perm::Players &players, perm::Ledger &ledger) {
  ledger.balance.resize(players.rows.size(), 0);
  std::set<PlayerId> initialized;
  for (const auto &row : plan.at("balances")) {
    const auto wallet = detail::account(row.at("holder"));
    if (wallet >= players.rows.size())
      throw std::invalid_argument("unknown initial player wallet");
    if (!row.at("amount").is_number_integer())
      throw std::invalid_argument("initial balance must be whole units");
    const auto amount = row.at("amount").get<std::int64_t>();
    if (amount < 0 || !initialized.insert(wallet).second)
      throw std::invalid_argument("invalid or duplicate initial balance");
    ledger.balance[wallet] = amount;
  }
}
void install_grants(const Json &plan, const Symbols &symbols, const react::ValueRead &values,
                    AuthorityRead authority, const perm::OwnerLimits &limits,
                    const catalog::Models &models, const catalog::Scripts &scripts,
                    const install::InstanceScripts &instances, const react::RuleVersions &rules,
                    const perm::Plots &plots, const control::Profiles &profiles,
                    perm::Grants &grants, perm::Players &players, Step now, double hz) {
  for (const auto &row : plan.at("grants")) {
    const auto &holder = row.at("holder");
    const auto object = symbols.objects.at(holder.at("on").get<std::string>());
    const auto script = symbols.scripts.at(holder.at("script").get<std::string>());
    if (!values.identity.valid(object) || !script || script >= scripts.rows.size())
      throw std::invalid_argument("unknown initial grant holder");
    auto installed = install::installed_scripts(models, instances, values.identity, object);
    const auto applicable = install::rule_scripts(rules, values, object, now, hz);
    installed.insert(installed.end(), applicable.begin(), applicable.end());
    const bool exists = std::find(installed.begin(), installed.end(), script) != installed.end();
    const auto actual_holder = perm::Holder::script(script, object);
    const perm::HolderControl holder_control{actual_holder, authority.owner[object.slot()], exists};
    const auto &scope = row.at("scope");
    perm::ScopeControl control;
    if (scope.contains("object")) {
      const auto endpoint = symbols.objects.at(scope.at("object").get<std::string>());
      const auto target = perm::target(values.identity, authority, limits, endpoint);
      control.scope = perm::Scope::object(endpoint);
      control.controller = target.owner;
      control.plot = target.plot;
      control.available = (perm::all_rights & ~perm::inherited_rights) |
                          (target.owner_limit & perm::inherited_rights);
    } else if (scope.contains("owned_on_plot")) {
      const auto &owned = scope.at("owned_on_plot");
      const auto owner = owned.at("owner").get<PlayerId>();
      const auto plot = owned.at("plot").get<PlotId>();
      if (!owner || owner >= players.rows.size() || !plot || plot >= plots.rows.size() ||
          plots.rows[plot].id != plot)
        throw std::invalid_argument("unknown owned-on-plot scope");
      control = {perm::Scope::owned_on_plot(owner, plot), owner, perm::all_rights, plot, false};
    } else if (scope.contains("account")) {
      const auto wallet = detail::account(scope.at("account"));
      if (wallet >= players.rows.size())
        throw std::invalid_argument("unknown initial player wallet scope");
      control = {perm::Scope::account(wallet), wallet, 1, 0, false};
    } else if (scope.contains("plot")) {
      const auto id = scope.at("plot").get<PlotId>();
      const auto &plot = plots.rows.at(id);
      if (!id || plot.id != id)
        throw std::invalid_argument("unknown initial plot scope");
      control = {perm::Scope::plot(id), plot.owner,
                 perm::Rights{7} | perm::bit(perm::Verb::Transfer), id, false};
    } else if (scope.value("issuance", false)) {
      const auto issuer = row.at("issuer").get<PlayerId>();
      const auto &profile = profiles.rows.at(issuer);
      const auto architect =
          std::find(profile.roles.begin(), profile.roles.end(), "architect") != profile.roles.end();
      control = {perm::Scope::issuance(), 0, 1, 0, architect};
    } else
      throw std::invalid_argument("unknown initial grant scope");
    const perm::GrantRequest request{row.at("issuer").get<PlayerId>(),
                                     actual_holder,
                                     control.scope,
                                     detail::rights(row.at("rights"), control.scope.kind),
                                     row.at("cap").get<std::int64_t>(),
                                     detail::steps(row.at("period_s").get<double>(), hz),
                                     now + detail::steps(row.at("expires_s").get<double>(), hz)};
    const auto created = perm::create_grant(grants, players, request, control, holder_control, now);
    if (!created)
      throw std::invalid_argument("initial grant denied by ordinary permissions: " +
                                  std::string(perm::reason_text(created.reason)));
  }
}
} // namespace amber::bindings::start
