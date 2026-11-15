#include "bindings/commerce/commerce.hpp"
#include "bindings/finance/finance.hpp"
#include "checked_actions_fixture.hpp"

#include <iostream>
#include <limits>

using namespace amber;
using test_runtime::require;
namespace {
struct Fixture {
  test_actions::Fixture world;
  perm::Ledger ledger;
  perm::Groups groups;
  bindings::commerce::BalanceBinding balance{ledger, world.players, identity(world.f.objects),
                                             authority(world.f.objects), groups};
  bindings::commerce::PaymentBinding payment{
      ledger, world.grants, world.players, identity(world.f.objects), authority(world.f.objects),
      groups};
  bindings::IssuanceBinding issuance{
      ledger, world.grants, world.players, identity(world.f.objects), authority(world.f.objects),
      groups};
  std::array<TermId, 4> triggers{};
  Fixture() {
    ledger.balance[perm::Account::player(2)] = 100;
    groups.rows.push_back({1, 2, "Institution"});
    world.f.objects.owner[world.f.a.slot()] = 2;
    world.f.objects.owner[world.f.b.slot()] = 2;
    world.f.objects.model[world.f.b.slot()] = 2;
    for (std::size_t i = 0; i < triggers.size(); ++i) {
      react::Term term;
      term.id = static_cast<TermId>(world.f.terms.rows.size());
      term.name = "arbitrary.finance" + std::to_string(i + 1);
      term.kind = react::Kind::Event;
      triggers[i] = term.id;
      world.f.terms.by_name.emplace(term.name, term.id);
      world.f.terms.rows.push_back(std::move(term));
    }
    const auto declarations =
        bindings::commerce::declare_commerce(world.f.terms, world.declarations);
    drive::register_native(world.native, declarations.pay,
                           {bindings::commerce::validate_pay, bindings::commerce::pay, &payment});
    drive::register_native(
        world.native, declarations.balance,
        {bindings::commerce::validate_balance, bindings::commerce::balance, &balance});
    bindings::register_issuance_action(world.f.terms, world.declarations, world.native, issuance);
    world.load(R"(return {kind='model',on={
      ['arbitrary.finance1']=function(self,_,args)
        local result=self:act('engine.pay',self.handle,
          {from=args.payer,to=args.to,amount=args.amount})
        assert(result.ok==args.expect)
        self.state.returned=true
      end,
      ['arbitrary.finance2']=function(self,_,args)
        local result=self:act('engine.issue',self.handle,{to=args.to,amount=args.amount})
        assert(result.ok==args.expect)
        self.state.returned=true
      end,
      ['arbitrary.finance3']=function(self,_,args)
        assert(self:act('engine.balance',nil,{}).balance==args.expected)
        assert(self:act('engine.balance',nil,{account='p:2'}).balance==args.explicit)
      end,
      ['arbitrary.finance4']=function(self,_,args)
        assert(self:act('engine.balance',nil,{account=args.wallet}).ok==args.expect)
      end,
    }})");
  }
  react::RunOutcome run(unsigned trigger, std::vector<EventField> args, Handle receiver = {},
                        Step step = 10) {
    world.clock.step = step;
    bindings::InvocationInput input;
    input.receiver = receiver ? receiver : world.f.a;
    input.trigger = triggers.at(trigger - 1);
    input.step = step;
    input.payload = std::move(args);
    input.action_caller = PlayerId{2};
    input.action_body = world.f.a;
    drive::PlannedInvocation plan;
    plan.handler.script = world.script;
    plan.handler.function = world.f.terms.rows[input.trigger].name;
    plan.input = std::move(input);
    auto completed = bindings::run_invocation(world.actions, plan);
    auto result = completed.outcome;
    bindings::publish_invocation(world.journal, world.queues, world.timers, std::move(completed),
                                 {}, 1000);
    return result;
  }
  void ok(unsigned trigger, std::vector<EventField> args, Handle receiver = {}, Step step = 10) {
    const auto outcome = run(trigger, std::move(args), receiver, step);
    require(outcome.status == react::RunOutcome::Status::Ok, outcome.error.c_str());
  }
  GrantId grant(perm::Scope scope, std::int64_t cap, bool architect = false) {
    const auto holder = perm::Holder::script(world.script, world.f.a);
    const auto granted = perm::create_grant(
        world.grants, world.players, perm::Holder::player(2), {2, holder, scope, 1, cap, 100, 1000},
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
  f.ok(3, {{"expected", Value(0.)}, {"explicit", Value(100.)}});
  f.ok(1, pay_args(20, false));
  require(perm::balance(f.ledger, perm::Account::player(2)) == 100 &&
              perm::balance(f.ledger, perm::Account::player(3)) == 0,
          "descriptive player/body identity did not authorize script spending");
  f.world.journal.take();
  const auto grant = f.grant(perm::Scope::account(perm::Account::player(2)), 40);
  f.ok(1, pay_args(34, true));
  require(perm::balance(f.ledger, perm::Account::player(2)) == 66 &&
              perm::balance(f.ledger, perm::Account::player(3)) == 34 &&
              f.world.grants.rows[grant].spent == 34 &&
              std::get<bool>(
                  f.world.f.states.at({f.world.f.a, f.world.script}).at(std::string("returned"))),
          "actual synchronous self.act payment did not return with its capped transfer applied");
  const auto logs = f.world.journal.take();
  require(logs.size() == 3 && logs[0]["kind"] == "handler" && logs[1]["name"] == "engine.pay" &&
              logs[1]["cause"] == logs[0]["id"] &&
              logs[1]["actor"] == "s:" + std::to_string(f.world.script) &&
              logs[1]["agent"] == false && logs[2]["name"] == "payment" &&
              logs[2]["cause"] == logs[1]["id"],
          "actual payment action/effect lost its handler cause or became another player choice");
  f.ok(1, pay_args(10, false));
  f.ok(1, pay_args(1, false), f.world.f.b);
  require(perm::balance(f.ledger, perm::Account::player(3)) == 34 &&
              f.world.grants.rows[grant].spent == 34,
          "cap failure and another installation preserve wallets and allowance");
  perm::revoke(f.world.grants, f.world.players, grant);
  f.ok(1, pay_args(1, false));
  const auto larger = f.grant(perm::Scope::account(perm::Account::player(2)), 1000);
  f.ok(1, pay_args(100, false));
  f.ok(1, pay_args(-1, false));
  f.ok(1, pay_args(.5, false));
  require(f.world.grants.rows[larger].spent == 0 &&
              perm::balance(f.ledger, perm::Account::player(2)) == 66,
          "invalid amount or insufficient funds consumed money/allowance");
  require(f.run(1, pay_args(std::numeric_limits<double>::infinity(), false)).status ==
              react::RunOutcome::Status::Error,
          "nonfinite action values must not cross the Lua JSON boundary");
}
void issuance_and_wallet_identity() {
  Fixture f;
  auto args = [](double amount, bool expect) {
    return std::vector<EventField>{
        {"to", std::string("p:3")}, {"amount", Value(amount)}, {"expect", Value(expect)}};
  };
  f.ok(2, args(10, false));
  const auto grant = f.grant(perm::Scope::issuance(), 30, true);
  f.ok(2, args(21, true));
  f.ok(2, args(10, false));
  require(perm::balance(f.ledger, perm::Account::player(3)) == 21 &&
              f.world.grants.rows[grant].spent == 21,
          "ordinary engine.issue did not enforce capped issuance authority");
  f.ok(2, args(20, true), {}, 110);
  require(perm::balance(f.ledger, perm::Account::player(3)) == 41 &&
              f.world.grants.rows[grant].spent == 20,
          "issuance period reset is applied once");
  f.ok(2, args(1, false), {}, 1000);
  require(perm::balance(f.ledger, perm::Account::player(3)) == 41, "expired issuance cannot mint");
  f.world.f.objects.owner[f.world.f.a.slot()] = 3;
  f.ok(3, {{"expected", Value(0.)}, {"explicit", Value(100.)}});
  for (const auto &account : {std::string("group:1"), to_string(f.world.f.a)})
    f.ok(4, {{"wallet", account}, {"expect", Value(true)}});
  for (const auto &account :
       {std::string("group:999"), std::string("p:999"),
        to_string(Handle::make(f.world.f.a.slot(), f.world.f.a.generation() + 1))})
    f.ok(4, {{"wallet", account}, {"expect", Value(false)}});
}
void object_and_group_payments_use_their_actual_accounts() {
  Fixture f;
  const auto object = perm::Account::object(f.world.f.a);
  const auto group = perm::Account::group(1);
  f.ledger.balance[object] = 40;
  f.ledger.balance[group] = 70;
  auto args = [](std::string from, std::string to, double amount, bool expect) {
    return std::vector<EventField>{{"payer", std::move(from)},
                                   {"to", std::move(to)},
                                   {"amount", Value(amount)},
                                   {"expect", Value(expect)}};
  };
  const auto name = to_string(f.world.f.a);
  f.ok(3, {{"expected", Value(40.)}, {"explicit", Value(100.)}});
  f.ok(1, args(name, "group:1", 15, false));
  const auto object_grant = f.grant(perm::Scope::account(object), 20);
  f.ok(1, args(name, "group:1", 15, true));
  f.ok(1, args(name, "group:1", 6, false));
  f.ok(1, args("group:1", name, 6, false));
  f.grant(perm::Scope::account(group), 10);
  f.ok(1, args("group:1", name, 6, true));
  f.grant(perm::Scope::issuance(), 20, true);
  f.ok(2, {{"to", name}, {"amount", Value(7.)}, {"expect", Value(true)}});
  f.ok(2, {{"to", std::string("group:1")}, {"amount", Value(8.)}, {"expect", Value(true)}});
  require(perm::balance(f.ledger, object) == 38 && perm::balance(f.ledger, group) == 87 &&
              perm::balance(f.ledger, perm::Account::player(2)) == 100,
          "actual Lua pay/issue aliased a typed account to the owner's wallet");
  perm::transfer_owner(authority(f.world.f.objects), f.world.limits,
                       permission_flags(f.world.f.objects), f.world.grants, f.world.players,
                       perm::target(identity(f.world.f.objects), authority(f.world.f.objects),
                                    f.world.limits, f.world.f.a),
                       3);
  f.ok(3, {{"expected", Value(38.)}, {"explicit", Value(100.)}});
  f.ok(1, args(name, "group:1", 1, false));
  require(!f.world.grants.rows[object_grant].live,
          "sale left an old account Spend grant installed");
}
} // namespace
int main() {
  script_spending();
  issuance_and_wallet_identity();
  object_and_group_payments_use_their_actual_accounts();
  std::cout << "Actual synchronous Lua actions enforce wallet, grant, cap and issuance semantics\n";
}
