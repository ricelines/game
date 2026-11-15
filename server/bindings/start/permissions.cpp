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
void install_money(const Json &plan, const Symbols &symbols, IdentityRead identity,
                   AuthorityRead authority, const perm::Players &players,
                   const perm::Groups &groups, perm::Ledger &ledger) {
  std::set<perm::Account> initialized;
  for (const auto &row : plan.at("balances")) {
    auto reference = row.at("holder");
    if (const auto found = symbols.objects.find(reference.get<std::string>());
        found != symbols.objects.end())
      reference = to_string(found->second);
    const auto account =
        authority::account(reference, {identity, authority, players, groups}).account;
    if (!row.at("amount").is_number_integer())
      throw std::invalid_argument("initial balance must be whole units");
    const auto amount = row.at("amount").get<std::int64_t>();
    if (amount < 0 || !initialized.insert(account).second)
      throw std::invalid_argument("invalid or duplicate initial balance");
    ledger.balance[account] = amount;
  }
}
void install_grants(const Json &plan, const Symbols &symbols, const react::ValueRead &values,
                    AuthorityRead authority, const perm::OwnerLimits &limits,
                    const catalog::Models &models, const catalog::Scripts &scripts,
                    const install::InstanceScripts &instances, const react::RuleVersions &rules,
                    const perm::Plots &plots, const control::Profiles &profiles,
                    perm::Grants &grants, perm::Players &players, Step now, double hz,
                    const perm::Groups &groups) {
  const authority::ScopeRead scope_read{values.identity, authority, limits, players,
                                        plots,           profiles,  groups};
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
    auto installed = install::installed_scripts(models, instances, values.identity, object);
    const auto applicable = install::rule_scripts(rules, values, object, now, hz);
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
    const auto control = authority::scope(scope, scope_read, issuer);
    const auto duration = detail::steps(row.value("expires_s", 0.), hz);
    const perm::GrantRequest request{issuer,
                                     recipient.holder,
                                     control.scope,
                                     authority::rights(row.at("rights"), control.scope.kind),
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
} // namespace amber::bindings::start
