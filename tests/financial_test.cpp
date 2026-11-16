#include "net_fixture.hpp"
#include "runtime_fixture.hpp"
#include "space_fixture.hpp"
#include "step/action_private.hpp"

#include <iostream>
#include <limits>

using namespace amber;
using test_runtime::require;
namespace {
struct Fixture {
  amber::perm::State perm_state;
  amber::net::State net_state{test_net::options()};
  amber::step::State step_state{8};

  test_runtime::Fixture f{1000};
  amber::ObjectState &objects_state = f.objects_state;

  amber::react::State &react_state = f.react_state;

  space::State space_state{testspace::settings(), 8};
  perm::Players &players = perm_state.players;
  perm::Grants &grants = perm_state.grants;
  perm::OwnerLimits &limits = perm_state.owner_limits;
  perm::Ledger &ledger = perm_state.ledger;
  perm::Groups &groups = perm_state.groups;
  net::Profiles &profiles = net_state.profiles;
  react::NativeActions &declarations = react_state.native_declarations;
  step::Intents &intents = step_state.intents;
  Relation &parts = objects_state.declared_parts;
  record::Journal journal{8, {}};
  step::WorkQueues &queues = step_state.queues;
  step::TimerWheel &timers = step_state.timers;
  std::array<TermId, 4> triggers{};
  ScriptId script = 0;
  react::Invocation invocation;
  react::CompletedInvocation completed;
  Step now = 10;
  Fixture() {
    players.rows.resize(4);
    profiles.rows.resize(4);
    for (PlayerId id = 1; id < 4; ++id)
      profiles.rows[id].id = id;
    ledger.balance[perm::Account::player(2)] = 100;
    groups.rows.push_back({1, 2, "Institution"});
    f.objects.owner[f.a.slot()] = 2;
    f.objects.owner[f.b.slot()] = 2;
    for (std::size_t i = 0; i < triggers.size(); ++i) {
      react::Term term;
      term.id = f.terms.rows.size();
      term.name = "arbitrary.finance" + std::to_string(i + 1);
      term.kind = react::Kind::Event;
      triggers[i] = term.id;
      f.terms.by_name.emplace(term.name, term.id);
      f.terms.rows.push_back(std::move(term));
    }
    step::declare_commerce(f.terms, declarations);
    step::declare_issuance_action(f.terms, declarations);
    script = f.add(R"(return {kind='model',on={
      ['arbitrary.finance1']=function(self,_,args)
        local result=self:act('engine.pay',self.handle,
          {from=args.payer,to=args.to,amount=args.amount})
        assert(result.ok==args.expect);self.state.returned=true
      end,
      ['arbitrary.finance2']=function(self,_,args)
        local result=self:act('engine.issue',self.handle,{to=args.to,amount=args.amount})
        assert(result.ok==args.expect);self.state.returned=true
      end,
      ['arbitrary.finance3']=function(self,_,args)
        assert(self:act('engine.balance',nil,{}).balance==args.expected)
        assert(self:act('engine.balance',nil,{account='p:2'}).balance==args.explicit)
      end,
      ['arbitrary.finance4']=function(self,_,args)
        assert(self:act('engine.balance',nil,{account=args.wallet}).ok==args.expect)
      end,
    }})",
                   2);
    f.models.rows[1].script = script;
    react::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  }
  react::RunOutcome start(unsigned trigger, std::vector<EventField> args, Handle receiver = {},
                          Step step = 10) {
    now = step;
    invocation = {};
    completed = {};
    react::InvocationInput input;
    input.receiver = receiver ? receiver : f.a;
    input.trigger = triggers.at(trigger - 1);
    input.step = step;
    input.hz = 20;
    input.payload = std::move(args);
    input.action_caller = PlayerId{2}; // display metadata never grants script Spend/Issue
    input.action_body = f.a;
    const auto handlers =
        react::dispatch(f.triggers, f.values(), input.receiver, input.trigger, step, 20);
    require(handlers.size() == 1, "financial fixture uses its actual installed handler");
    react::start_invocation(f.react_state, objects_state, 7, f.catalog_state, journal,
                            handlers.front(), std::move(input), invocation, completed);
    return completed.outcome;
  }
  step::ActionRun emitted() {
    require(completed.outcome.status == react::RunOutcome::Status::Action &&
                completed.outcome.action,
            "real Lua reaches its explicit ordinary action call");
    step::NativeCall call;
    call.request = completed.outcome.action->request;
    call.arguments = completed.outcome.action->arguments;
    call.step = now;
    call.hz = 20;
    call.origin = invocation.origin;
    call.deadline = invocation.deadline;
    auto run =
        step::begin_action(f.catalog_state, f.react_state, profiles, perm_state, f.objects_state,
                           space_state, intents, journal, 2, std::move(call));
    require(!run.outcome, "the actual installed script's finance action is admitted");
    return run;
  }
  void resume(step::ActionRun &run, step::NativeCompletion result) {
    const auto reply = step::finish_native(run, result, identity(f.objects), attribution(f.objects),
                                           f.models, f.scripts, journal);
    for (auto &event : result.records)
      journal.append(std::move(event), reply.origin);
    completed.outcome = react::resume_handler(f.vm, invocation, reply.reply);
  }
  void finish() {
    react::finish_invocation(identity(f.objects), attribution(f.objects), f.models, f.scripts,
                             journal, invocation, completed);
    require(completed.outcome.status == react::RunOutcome::Status::Ok,
            completed.outcome.error.c_str());
    step::publish_effects(std::move(completed.effects), completed.origin, now, queues, timers,
                          journal, 1000);
  }
  void pay(std::vector<EventField> args, Handle receiver = {}, Step step = 10) {
    start(1, std::move(args), receiver, step);
    auto run = emitted();
    resume(run, step::pay(ledger, grants, players, identity(f.objects), authority(f.objects),
                          groups, run.call));
    finish();
  }
  void issue(std::vector<EventField> args, Handle receiver = {}, Step step = 10) {
    start(2, std::move(args), receiver, step);
    auto run = emitted();
    resume(run, step::issue(ledger, grants, players, identity(f.objects), authority(f.objects),
                            groups, run.call));
    finish();
  }
  void balances(std::vector<EventField> args) {
    start(3, std::move(args));
    auto own = emitted();
    resume(own, step::balance(ledger, players, identity(f.objects), authority(f.objects), groups,
                              own.call));
    auto explicit_account = emitted();
    resume(explicit_account, step::balance(ledger, players, identity(f.objects),
                                           authority(f.objects), groups, explicit_account.call));
    finish();
  }
  void wallet(std::vector<EventField> args) {
    start(4, std::move(args));
    auto run = emitted();
    resume(run, step::balance(ledger, players, identity(f.objects), authority(f.objects), groups,
                              run.call));
    finish();
  }
  GrantId grant(perm::Scope scope, std::int64_t cap, bool architect = false) {
    const auto holder = perm::Holder::script(script, f.a);
    const auto granted = perm::create_grant(
        grants, players, perm::Holder::player(2), {2, holder, scope, 1, cap, 100, 1000},
        {scope, 2, perm::all_rights, 0, architect}, {holder, 2, true}, 0);
    require(bool(granted), "ordinary capped script grant");
    return granted.grant;
  }
};
std::vector<EventField> pay_args(double amount, bool expect) {
  return {{"payer", std::string("p:2")},
          {"to", std::string("p:3")},
          {"amount", Value(amount)},
          {"expect", Value(expect)}};
}
void script_spending() {
  Fixture f;
  f.balances({{"expected", Value(0.)}, {"explicit", Value(100.)}});
  f.pay(pay_args(20, false));
  require(perm::balance(f.ledger, perm::Account::player(2)) == 100 &&
              perm::balance(f.ledger, perm::Account::player(3)) == 0,
          "descriptive player/body identity did not authorize script spending");
  f.journal.take();
  const auto grant = f.grant(perm::Scope::account(perm::Account::player(2)), 40);
  f.pay(pay_args(34, true));
  require(perm::balance(f.ledger, perm::Account::player(2)) == 66 &&
              perm::balance(f.ledger, perm::Account::player(3)) == 34 &&
              f.grants.rows[grant].spent == 34 &&
              std::get<bool>(f.f.states.at({f.f.a, f.script}).at(std::string("returned"))),
          "actual synchronous self.act payment did not return with its capped transfer applied");
  const auto logs = f.journal.take();
  require(logs.size() == 3 && logs[0]["kind"] == "handler" && logs[1]["name"] == "engine.pay" &&
              logs[1]["cause"] == logs[0]["id"] &&
              logs[1]["actor"] == "s:" + std::to_string(f.script) && logs[1]["agent"] == false &&
              logs[2]["name"] == "payment" && logs[2]["cause"] == logs[1]["id"],
          "actual payment action/effect lost its handler cause or became another player choice");
  f.pay(pay_args(10, false));
  f.pay(pay_args(1, false), f.f.b);
  require(perm::balance(f.ledger, perm::Account::player(3)) == 34 &&
              f.grants.rows[grant].spent == 34,
          "cap failure and another installation preserve wallets and allowance");
  perm::revoke(f.grants, f.players, grant);
  f.pay(pay_args(1, false));
  const auto larger = f.grant(perm::Scope::account(perm::Account::player(2)), 1000);
  f.pay(pay_args(100, false));
  for (const auto &amount : {nlohmann::json(-1), nlohmann::json(.5)}) {
    const nlohmann::json arguments{{"from", "p:2"}, {"to", "p:3"}, {"amount", amount}};
    require(!step::validate_pay(arguments).empty(),
            "the production payment boundary rejects negative or fractional currency");
    require(!step::validate_issue({{"to", "p:3"}, {"amount", amount}}).empty(),
            "the production issuance boundary rejects negative or fractional currency");
  }
  require(f.grants.rows[larger].spent == 0 &&
              perm::balance(f.ledger, perm::Account::player(2)) == 66,
          "invalid amount or insufficient funds consumed money/allowance");
  require(f.start(1, pay_args(std::numeric_limits<double>::infinity(), false)).status ==
              react::RunOutcome::Status::Error,
          "nonfinite action values must not cross the Lua JSON boundary");
}
void issuance_and_wallet_identity() {
  Fixture f;
  auto args = [](double amount, bool expect) {
    return std::vector<EventField>{
        {"to", std::string("p:3")}, {"amount", Value(amount)}, {"expect", Value(expect)}};
  };
  f.issue(args(10, false));
  const auto grant = f.grant(perm::Scope::issuance(), 30, true);
  f.issue(args(21, true));
  f.issue(args(10, false));
  require(perm::balance(f.ledger, perm::Account::player(3)) == 21 &&
              f.grants.rows[grant].spent == 21,
          "ordinary engine.issue did not enforce capped issuance authority");
  f.issue(args(20, true), {}, 110);
  require(perm::balance(f.ledger, perm::Account::player(3)) == 41 &&
              f.grants.rows[grant].spent == 20,
          "issuance period reset is applied once");
  f.issue(args(1, false), {}, 1000);
  require(perm::balance(f.ledger, perm::Account::player(3)) == 41, "expired issuance cannot mint");
  f.f.objects.owner[f.f.a.slot()] = 3;
  f.balances({{"expected", Value(0.)}, {"explicit", Value(100.)}});
  for (const auto &account : {std::string("group:1"), to_string(f.f.a)})
    f.wallet({{"wallet", account}, {"expect", Value(true)}});
  for (const auto &account : {std::string("group:999"), std::string("p:999"),
                              to_string(Handle::make(f.f.a.slot(), f.f.a.generation() + 1))}) {
    step::NativeCall call;
    call.request.caller = ScriptHolder{f.script, f.f.a};
    call.arguments = {{"account", account}};
    bool rejected = false;
    try {
      step::balance(f.ledger, f.players, identity(f.f.objects), authority(f.f.objects), f.groups,
                    call);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    require(rejected, "unknown or stale accounts cannot be read as an empty wallet");
  }
}
void object_and_group_payments_use_their_actual_accounts() {
  Fixture f;
  const auto object = perm::Account::object(f.f.a);
  const auto group = perm::Account::group(1);
  f.ledger.balance[object] = 40;
  f.ledger.balance[group] = 70;
  auto args = [](std::string from, std::string to, double amount, bool expect) {
    return std::vector<EventField>{{"payer", std::move(from)},
                                   {"to", std::move(to)},
                                   {"amount", Value(amount)},
                                   {"expect", Value(expect)}};
  };
  const auto name = to_string(f.f.a);
  f.balances({{"expected", Value(40.)}, {"explicit", Value(100.)}});
  f.pay(args(name, "group:1", 15, false));
  const auto object_grant = f.grant(perm::Scope::account(object), 20);
  f.pay(args(name, "group:1", 15, true));
  f.pay(args(name, "group:1", 6, false));
  f.pay(args("group:1", name, 6, false));
  f.grant(perm::Scope::account(group), 10);
  f.pay(args("group:1", name, 6, true));
  f.grant(perm::Scope::issuance(), 20, true);
  f.issue({{"to", name}, {"amount", Value(7.)}, {"expect", Value(true)}});
  f.issue({{"to", std::string("group:1")}, {"amount", Value(8.)}, {"expect", Value(true)}});
  require(perm::balance(f.ledger, object) == 38 && perm::balance(f.ledger, group) == 87 &&
              perm::balance(f.ledger, perm::Account::player(2)) == 100,
          "actual Lua pay/issue aliased a typed account to the owner's wallet");
  perm::transfer_owner(
      authority(f.f.objects), f.limits, permission_flags(f.f.objects), f.grants, f.players,
      perm::target(identity(f.f.objects), authority(f.f.objects), f.limits, f.f.a), 3);
  f.balances({{"expected", Value(38.)}, {"explicit", Value(100.)}});
  f.pay(args(name, "group:1", 1, false));
  require(!f.grants.rows[object_grant].live, "sale left an old account Spend grant installed");
}
} // namespace
int main() {
  script_spending();
  issuance_and_wallet_identity();
  object_and_group_payments_use_their_actual_accounts();
  std::cout << "Actual synchronous Lua actions enforce wallet, grant, cap and issuance semantics\n";
}
