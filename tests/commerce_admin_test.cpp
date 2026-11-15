#include "runtime_fixture.hpp"
#include "step/action_private.hpp"

#include <chrono>
#include <iostream>

using namespace amber;
using test_runtime::require;
namespace ad = amber::step;
using Json = nlohmann::json;
namespace {
step::NativeCall call(PlayerId player, TermId action, Handle target, Json arguments) {
  return {{player, action, target, {}}, std::move(arguments), {}, 10, 20, {}};
}
GrantId grant_id(const step::NativeCompletion &result) {
  require(result.reply.value("ok", false), result.reply.dump().c_str());
  return step::identifier(result.reply.at("grant"), "g:");
}
} // namespace
int main() {
  try {
    test_runtime::Fixture f;
    amber::perm::State perm_state;
    perm::Players &players = perm_state.players;
    players.rows.resize(5);
    perm::Groups &groups = perm_state.groups;
    perm::Grants &grants = perm_state.grants;
    perm::OwnerLimits &limits = perm_state.owner_limits;
    perm::Plots &plots = perm_state.plots;
    plots.rows.push_back({1, 1, 0, 0, {-10, -10, 10, 10}, {}});
    f.objects.owner[f.a.slot()] = 1;
    f.objects.owner[f.b.slot()] = 2;
    f.objects.owner[f.c.slot()] = 3;
    f.objects.plot[f.a.slot()] = 1;
    f.objects.plot[f.b.slot()] = 1;
    f.objects.plot[f.c.slot()] = 1;
    const auto values = f.values();
    react::NativeActions native;
    const auto ids = ad::declare_admin(f.terms, native);
    const auto count = f.terms.rows.size();
    require(ad::declare_admin(f.terms, native).grant == ids.grant && f.terms.rows.size() == count &&
                native.rows.size() == 7,
            "administration declarations changed registry identities on restore");
    auto invoke = [&](step::NativeCall input) {
      const auto *player = std::get_if<PlayerId>(&input.request.caller);
      const bool architect = player && *player == 4;
      return step::native_permissions(
          f.catalog_state, values, f.react_state, perm_state, f.objects_state, architect,
          std::get<react::PermissionAction>(native.rows.at(input.request.action).operation), input);
    };
    auto run = [&](PlayerId player, TermId action, Handle object, Json input) {
      return invoke(call(player, action, object, std::move(input)));
    };
    require(!ad::validate_grant({{"holder", "p:2"},
                                 {"scope", {{"account", "p:1"}}},
                                 {"rights", {"spend"}},
                                 {"unexpected", 1}})
                    .empty() &&
                ad::validate_revoke({{"grant", "g:1"}}).empty() &&
                !ad::validate_revoke({{"grant", "grant:1"}}).empty() &&
                !ad::validate_revoke({{"grant", "g:1"}, {"holder", "p:2"}}).empty() &&
                !ad::validate_plot_permissions({{"plot", -1}}).empty() &&
                !ad::validate_group_create({{"name", " \t"}}).empty() &&
                !ad::validate_group_member({{"group", "g:1"}, {"player", "p:2"}, {"member", true}})
                     .empty() &&
                !ad::validate_preset({{"group", "g:1"}}).empty() &&
                native.rows.at(ids.grant).inputs_schema["properties"]["holder"].contains("oneOf"),
            "structured administration schemas accepted malformed inputs or hid holder shape");
    const auto created_group = run(1, ids.group_create, f.a, {{"name", "Independent workshop"}});
    require(created_group.changed && created_group.reply.at("group") == "group:1" &&
                groups.rows[1].controller == 1 && players.rows[1].groups == std::vector<GroupId>{1},
            "group creation failed to install actual controller/membership");
    require(
        !run(2, ids.group_member, f.b, {{"group", "group:1"}, {"player", "p:2"}, {"member", true}})
             .reply["ok"],
        "non-controller added themself to group");
    require(
        run(1, ids.group_member, f.a, {{"group", "group:1"}, {"player", "p:2"}, {"member", true}})
                .changed &&
            !run(1, ids.group_member, f.a,
                 {{"group", "group:1"}, {"player", "p:2"}, {"member", true}})
                 .changed &&
            players.rows[2].groups == std::vector<GroupId>{1},
        "controller membership mutation was not sorted/idempotent");
    auto settings = run(1, ids.preset, f.a,
                        {{"everyone", {"use"}},
                         {"group_rights", {"edit"}},
                         {"group", "group:1"},
                         {"next_owner", {"use", "transfer"}},
                         {"no_copy", true}});
    require(settings.changed && settings.reply.at("preset").at("group") == "group:1" &&
                f.objects.group[f.a.slot()] == 1 && (f.objects.flags[f.a.slot()] & NoCopy) &&
                perm::check(perm::Holder::player(2), perm::Verb::Edit,
                            perm::target(identity(f.objects), authority(f.objects), limits, f.a),
                            players, grants, 10) &&
                !perm::check(perm::Holder::player(2), perm::Verb::Copy,
                             perm::target(identity(f.objects), authority(f.objects), limits, f.a),
                             players, grants, 10),
            "actual group preset/no-copy checks disagree with administration result");
    const auto not_owner = run(2, ids.preset, f.a, {{"everyone", {"destroy"}}}).reply;
    require(not_owner.at("ok") == false && not_owner.at("denied").at("need") == "owner" &&
                not_owner.at("denied").at("on") == to_string(f.a) &&
                not_owner.at("denied").at("fix").get<std::string>().find("p:1") !=
                    std::string::npos &&
                !not_owner.contains("problem"),
            "owner denial must identify the actual goods, missing authority and useful fix");
    limits.set(f.c.slot(), perm::bit(perm::Verb::Transfer));
    const auto restricted_before = f.objects.preset[f.c.slot()];
    require(!run(3, ids.preset, f.c, {{"everyone", {"copy"}}}).reply["ok"] &&
                !run(3, ids.preset, f.c, {{"no_copy", false}}).reply["ok"] &&
                f.objects.preset[f.c.slot()] == restricted_before,
            "owner widened an inherited restriction or denied preset partially mutated");
    const auto before_flags = f.objects.flags[f.a.slot()] |= Escrow;
    require(!run(1, ids.preset, f.a, {{"no_copy", false}}).reply["ok"] &&
                f.objects.flags[f.a.slot()] == before_flags,
            "escrow preset edit changed valuable flags");
    f.objects.flags[f.a.slot()] &= ~Escrow;
    const auto preset_before = f.objects.preset[f.b.slot()];
    require(
        !run(2, ids.preset, f.b, {{"everyone", {"destroy"}}, {"group", "group:999"}}).reply["ok"] &&
            f.objects.preset[f.b.slot()] == preset_before,
        "invalid group assignment partially committed preset bits");
    require(run(1, ids.plot_permissions, f.a,
                {{"plot", 1},
                 {"everyone", {"enter"}},
                 {"group_rights", {"build"}},
                 {"group", "group:1"},
                 {"banned", {"p:3", "p:2", "p:3"}}})
                    .changed &&
                plots.rows[1].banned == std::vector<PlayerId>({2, 3}) &&
                perm::check_land(perm::Holder::player(4), perm::LandVerb::Enter, plots.rows[1],
                                 players, grants, 10) &&
                perm::check_land(perm::Holder::player(2), perm::LandVerb::Build, plots.rows[1],
                                 players, grants, 10)
                        .reason == perm::Reason::Banned,
            "plot permissions/bans failed their actual land checks");
    require(
        !run(2, ids.plot_permissions, f.b, {{"plot", 1}, {"banned", Json::array()}}).reply["ok"] &&
            !run(1, ids.plot_permissions, f.a, {{"plot", 1}, {"everyone", {"transfer"}}})
                 .reply["ok"],
        "plot preset accepted non-owner edit or invented a public land-transfer bit");
    require(
        run(2, ids.group_member, f.b, {{"group", "group:1"}, {"player", "p:2"}, {"member", false}})
                .changed &&
            players.rows[2].groups.empty(),
        "member could not leave their own group");
    require(run(2, ids.preset, f.b, {{"group", "group:1"}}).reply.at("denied").at("need") ==
                "group_membership",
            "former member assigned group they no longer belong to");
    const auto script = f.add("return {kind='instance',on={}}", 2);
    f.models.rows[1].script = script;
    const Json script_holder = {{"script", "s:" + std::to_string(script)}, {"on", to_string(f.b)}};
    const auto parent = grant_id(run(1, ids.grant, f.a,
                                     {{"holder", "p:2"},
                                      {"scope", {{"account", "p:1"}}},
                                      {"rights", {"spend"}},
                                      {"cap", 100},
                                      {"period_s", 60},
                                      {"expires_s", 120}}));
    const auto child = grant_id(run(2, ids.grant, f.b,
                                    {{"holder", script_holder},
                                     {"scope", {{"account", "p:1"}}},
                                     {"rights", {"spend"}},
                                     {"cap", 70},
                                     {"period_s", 30},
                                     {"expires_s", 60}}));
    require(grants.rows[child].parent == parent && grants.rows[child].expires_step == 1210,
            "delegated grant lost actual parent or expiry");
    const auto count_before = grants.rows.size();
    require(!run(2, ids.grant, f.b,
                 {{"holder", {{"script", "s:" + std::to_string(script)}, {"on", to_string(f.c)}}},
                  {"scope", {{"account", "p:1"}}},
                  {"rights", {"spend"}},
                  {"cap", 60},
                  {"period_s", 30},
                  {"expires_s", 60}})
                    .reply["ok"] &&
                grants.rows.size() == count_before,
            "delegation reached a script not controlled by the issuer");
    const auto uninstalled = f.add("return {kind='instance',on={}}", 1);
    require(
        !run(1, ids.grant, f.a,
             {{"holder", {{"script", "s:" + std::to_string(uninstalled)}, {"on", to_string(f.a)}}},
              {"scope", {{"object", to_string(f.a)}}},
              {"rights", {"edit"}},
              {"expires_s", 60}})
             .reply["ok"],
        "transport claimed an uninstalled script holder");
    require(
        !run(1, ids.grant, f.a,
             {{"holder", script_holder},
              {"scope", {{"object", to_string(f.a)}}},
              {"rights", {"edit"}}})
                .reply["ok"] &&
            !run(3, ids.grant, f.c,
                 {{"holder", "p:2"}, {"scope", {{"object", to_string(f.c)}}}, {"rights", {"copy"}}})
                 .reply["ok"],
        "script grant omitted expiry or inherited ceiling was widened");
    perm::Ledger &ledger = perm_state.ledger;
    ledger.balance[perm::Account::player(1)] = 200;
    require(bool(perm::pay(ledger, grants, perm::Holder::script(script, f.b),
                           perm::AccountTarget::player(1), perm::Account::player(3), 30, 10)),
            "actual delegated script could not spend its allowance");
    const auto object_account = perm::Account::object(f.a);
    const auto group_account = perm::Account::group(1);
    ledger.balance[object_account] = 40;
    ledger.balance[group_account] = 40;
    require(!perm::pay(ledger, grants, perm::Holder::script(script, f.b), {object_account, 1},
                       perm::Account::player(3), 1, 10) &&
                !perm::pay(ledger, grants, perm::Holder::script(script, f.b), {group_account, 1},
                           perm::Account::player(3), 1, 10),
            "player-wallet Spend grant must not alias object/group accounts");
    const auto object_grant = grant_id(
        run(1, ids.grant, f.a,
            {{"holder", {{"script", "s:" + std::to_string(script)}, {"on", to_string(f.b)}}},
             {"scope", {{"account", to_string(f.a)}}},
             {"rights", {"spend"}},
             {"cap", 5},
             {"period_s", 60},
             {"expires_s", 30}}));
    require(perm::pay(ledger, grants, perm::Holder::script(script, f.b), {object_account, 1},
                      perm::Account::player(3), 5, 10) &&
                ledger.balance.at(object_account) == 35 && ledger.balance.at(group_account) == 40 &&
                ledger.balance.at(perm::Account::player(1)) == 170,
            "ordinary account grant must debit exactly the selected object's bounded balance");
    require(run(1, ids.revoke, f.a, {{"grant", "g:" + std::to_string(object_grant)}}).changed,
            "explicit object-account grant revocation");
    const auto group_denial = run(2, ids.grant, f.b,
                                  {{"holder", "p:2"},
                                   {"scope", {{"account", "group:1"}}},
                                   {"rights", {"spend"}},
                                   {"cap", 5},
                                   {"period_s", 60},
                                   {"expires_s", 30}});
    require(group_denial.reply.contains("denied"),
            "group account uses actual controller authority rather than membership or ID alias");
    auto listed = run(2, ids.list_grants, f.b, Json::object());
    require(listed.reply["held"].size() == 1 && listed.reply["held"][0]["remaining"] == 70 &&
                listed.reply["given"].size() == 1 && listed.reply["given"][0]["remaining"] == 40 &&
                run(3, ids.list_grants, f.c, Json::object()).reply["held"].empty(),
            "indexed grants leaked foreign rows or reported the wrong shared allowance");
    auto after_expiry = call(2, ids.list_grants, f.b, Json::object());
    after_expiry.step = grants.rows[parent].expires_step;
    require(invoke(after_expiry).reply["held"].empty(),
            "listing reported expired grant before cleanup");
    require(!run(3, ids.revoke, f.c, {{"grant", "g:" + std::to_string(parent)}}).reply["ok"] &&
                grants.rows[parent].live,
            "foreign caller revoked another issuer's grant");
    const auto revoked = run(4, ids.revoke, f.c, {{"grant", "g:" + std::to_string(parent)}});
    require(revoked.changed && revoked.reply["revoked"].size() == 2 && !grants.rows[parent].live &&
                !grants.rows[child].live && players.rows[2].grants_held.empty() &&
                players.rows[2].grants_given.empty(),
            "architect revocation did not remove delegated children/indexes");
    const auto own = grant_id(
        run(1, ids.grant, f.a,
            {{"holder", "p:2"}, {"scope", {{"object", to_string(f.a)}}}, {"rights", {"use"}}}));
    const auto other = grant_id(
        run(3, ids.grant, f.c,
            {{"holder", "p:2"}, {"scope", {{"object", to_string(f.c)}}}, {"rights", {"use"}}}));
    require(run(1, ids.revoke, f.a, {{"holder", "p:2"}}).changed && !grants.rows[own].live &&
                grants.rows[other].live,
            "holder revocation removed a different issuer's grants");
    auto script_call = call(1, ids.group_create, f.a, {{"name", "No borrowed controller"}});
    script_call.request.caller = ScriptHolder{script, f.a};
    const auto player_only = invoke(script_call).reply;
    require(player_only.at("ok") == false && player_only.at("denied").at("need") == "player_caller",
            "script caller receives the actual player-only policy denial");
    auto script_run = [&](ScriptHolder actor, TermId action, Json arguments) {
      auto input = call(1, action, actor.object, std::move(arguments));
      input.request.caller = actor;
      return invoke(std::move(input));
    };
    require(!script_run({script, f.c}, ids.grant,
                        {{"holder", "p:1"},
                         {"scope", {{"object", to_string(f.c)}}},
                         {"rights", {"use"}},
                         {"expires_s", 30}})
                 .reply["ok"],
            "script issuing a grant inherited its object's owner's authority");
    const auto institution = grant_id(
        run(1, ids.grant, f.a,
            {{"holder", {{"script", "s:" + std::to_string(script)}, {"on", to_string(f.a)}}},
             {"scope", {{"plot", 1}}},
             {"rights", {"build", "enter", "run_scripts"}},
             {"expires_s", 120}}));
    const auto lease = grant_id(script_run({script, f.a}, ids.grant,
                                           {{"holder", "p:2"},
                                            {"scope", {{"plot", 1}}},
                                            {"rights", {"build", "enter"}},
                                            {"expires_s", 60}}));
    require(grants.rows[lease].parent == institution &&
                perm::check_scope(perm::Holder::player(2), perm::Scope::plot(1), 3, grants, 10),
            "ordinary institutional script could not delegate its explicit grant to a tenant");
    const auto listing = script_run({script, f.a}, ids.list_grants, Json::object()).reply;
    require(listing["held"].size() == 1 &&
                listing["held"][0]["id"] == "g:" + std::to_string(institution) &&
                listing["given"].size() == 1 &&
                listing["given"][0]["id"] == "g:" + std::to_string(lease),
            "script grant listing borrowed owner's grants or lost its actual issued child");
    require(!script_run({script, f.c}, ids.revoke, {{"grant", "g:" + std::to_string(lease)}})
                    .reply["ok"] &&
                grants.rows[lease].live,
            "a foreign script revoked the institution's child grant");
    require(script_run({script, f.a}, ids.revoke, {{"holder", "p:2"}, {"scope", {{"plot", 1}}}})
                    .changed &&
                !grants.rows[lease].live && grants.rows[institution].live,
            "holder+scope revocation missed the exact child or revoked its parent");
    require(!script_run({script, f.a}, ids.revoke, {{"holder", "p:2"}, {"scope", {{"plot", 1}}}})
                 .changed,
            "repeated exact-scope revocation invented a mutation");
    require(script_run({script, f.a}, ids.revoke, {{"grant", "g:" + std::to_string(institution)}})
                .changed,
            "script could not relinquish its own held root grant");
    f.objects.flags[f.b.slot()] |= Body;
    const auto seat = f.add("return {kind='instance',on={}}", 3);
    const std::array<ScriptId, 1> seats{seat};
    react::set_instances(f.installed, f.c, seats, f.scripts);
    const auto consent = grant_id(run(2, ids.grant, f.b,
                                      {{"holder", script_holder},
                                       {"scope", {{"avatar", to_string(f.b)}}},
                                       {"rights", {"animate", "camera"}},
                                       {"expires_s", 120}}));
    const Json seat_holder = {{"script", "s:" + std::to_string(seat)}, {"on", to_string(f.c)}};
    const auto seated = grant_id(script_run({script, f.b}, ids.grant,
                                            {{"holder", seat_holder},
                                             {"scope", {{"avatar", to_string(f.b)}}},
                                             {"rights", {"animate", "camera"}},
                                             {"expires_s", 60}}));
    require(grants.rows[seated].parent == consent &&
                perm::check_scope(perm::Holder::script(seat, f.c), perm::Scope::avatar(f.b), 3,
                                  grants, 10),
            "ordinary body script could not delegate its explicit consent to a foreign seat");
    require(script_run({script, f.b}, ids.revoke,
                       {{"holder", seat_holder}, {"scope", {{"avatar", to_string(f.b)}}}})
                    .changed &&
                !grants.rows[seated].live && grants.rows[consent].live,
            "standing-style exact revocation removed unrelated avatar consent");
    const auto rule = f.add(R"(return {kind='rule',applies_to={flag='novel.ready'},
      tests={{given={subject='target',state={['novel.ready']=true}},
        when={action={term='novel.touch',target='target'}},
        expect={subject='target',term='novel.ready',equals=true}}},
      on={['novel.cue']=function(self)end}})");
    f.state(f.a, 5, 1);
    react::add_rule(f.rules, f.scripts, f.terms, rule);
    const auto rule_grant =
        run(1, ids.grant, f.a,
            {{"holder", {{"script", "s:" + std::to_string(rule)}, {"on", to_string(f.a)}}},
             {"scope", {{"object", to_string(f.a)}}},
             {"rights", {"destroy"}},
             {"expires_s", 60}});
    require(rule_grant.changed, "actual applicable approved rule could not receive ordinary grant");
    f.state(f.a, 5, 0);
    require(!run(1, ids.grant, f.a,
                 {{"holder", {{"script", "s:" + std::to_string(rule)}, {"on", to_string(f.a)}}},
                  {"scope", {{"plot", 1}}},
                  {"rights", {"build"}},
                  {"expires_s", 60}})
                 .reply["ok"],
            "inapplicable world rule received holder authority");
    const auto stale = f.a;
    destroy(f.objects, f.a);
    require(!run(1, ids.preset, stale, {{"everyone", {"use"}}}).reply["ok"],
            "preset edit accepted a stale generation");
    require(created_group.permissions_changed && settings.permissions_changed,
            "successful permission edits omitted their actual invalidation delta");
    const auto begin = std::chrono::steady_clock::now();
    std::size_t total_rows = 0;
    for (unsigned iteration = 0; iteration < 1000; ++iteration)
      total_rows += run(2, ids.list_grants, f.b, Json::object()).reply["held"].size();
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    require(total_rows == 1000, "indexed listing benchmark lost its actual live row");
    const auto expiring = grant_id(run(1, ids.grant, f.b,
                                       {{"holder", "p:3"},
                                        {"scope", {{"account", "p:1"}}},
                                        {"rights", {"spend"}},
                                        {"cap", 20},
                                        {"period_s", 60},
                                        {"expires_s", 5}}));
    auto reminders = ad::advance_grants(grants, players, 90, 20, 20, 16);
    require(reminders.notices.size() == 1 && reminders.notices[0].recipient == 1 &&
                reminders.notices[0].event["kind"] == "grant_expiring" &&
                reminders.notices[0].event["id"] == "g:" + std::to_string(expiring) &&
                reminders.records.size() == 1 && !reminders.records[0].changed &&
                reminders.records[0].detail["agent"] == false &&
                reminders.records[0].detail["grant"] == "g:" + std::to_string(expiring),
            "real maintenance did not deliver the issuer's exact expiring grant");
    require(ad::advance_grants(grants, players, 91, 20, 20, 16).notices.empty(),
            "real maintenance repeated a grant reminder");
    auto expired = ad::advance_grants(grants, players, 110, 20, 20, 16);
    require(expired.notices.size() == 1 && expired.notices[0].recipient == 1 &&
                expired.notices[0].event["kind"] == "grant_expired" &&
                expired.notices[0].event["expires_in_s"] == 0 && !grants.rows[expiring].live &&
                expired.records.size() == 1 && expired.records[0].changed &&
                expired.records[0].detail["agent"] == false,
            "real maintenance did not expire authority and invalidate permission views");
    record::Journal journal(8, {});
    for (auto &event : reminders.records)
      journal.append(std::move(event));
    for (auto &event : expired.records)
      journal.append(std::move(event));
    const auto written = journal.take();
    require(written.size() == 2 && written[0]["id"] == 1 && written[0]["kind"] == "system" &&
                written[0]["grant"] == "g:" + std::to_string(expiring) && written[1]["id"] == 2 &&
                written[1]["name"] == "grant_expired" && written[1]["agent"] == false,
            "grant deadline producer overwrote reserved S7 metadata");
    std::cout << "commerce administration passed: native discovery/validation, real installed and "
                 "rule holders, shared caps, nested revocation, inherited presets, groups, land "
                 "bans, stale/script denials; indexed listing mean "
              << elapsed << " us\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
