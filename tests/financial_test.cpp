#include "bindings/finance/finance.hpp"

#include <iostream>
#include <stdexcept>

using namespace amber;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Fixture {
  Objects objects;
  perm::Players players;
  perm::Ledger ledger;
  perm::Grants grants;
  react::Terms terms;
  react::Vm vm{{4096, 50, 5, 64}};
  Handle a, b;
  react::ScriptState state;
  react::DeferredEffects effects;
  std::uint64_t rng = 1;
  bindings::BalanceBinding balance;
  bindings::PaymentBinding payment;
  bindings::IssuanceBinding issuance;
  Fixture()
      : balance{ledger, identity(objects), authority(objects), players},
        payment{ledger, grants, players}, issuance{ledger, grants, players} {
    players.rows.resize(4);
    ledger.balance.resize(4);
    ledger.balance[2] = 100;
    ObjectInit init;
    init.model = 1;
    init.owner = 2;
    a = create(objects, init);
    b = create(objects, init);
    terms.rows.resize(5);
    for (TermId i = 1; i < terms.rows.size(); ++i) {
      terms.rows[i].id = i;
      terms.rows[i].name = "arbitrary." + std::to_string(i);
      terms.rows[i].kind = react::Kind::Event;
      terms.by_name[terms.rows[i].name] = i;
    }
    const auto source = R"(
return {kind='instance', on={
  ['arbitrary.1']=function(self, _, args)
    local result=self:pay(args.payer,args.to,args.amount)
    assert(result.ok==args.expect,result.reason)
  end,
  ['arbitrary.2']=function(self, _, args)
    local result=self:issue(args.to,args.amount)
    assert(result.ok==args.expect,result.reason)
  end,
  ['arbitrary.3']=function(self, _, args)
    assert(self:balance()==args.expected)
    assert(self:balance('p:2')==args.explicit)
  end,
  ['arbitrary.4']=function(self, _, args) self:balance(args.wallet) end,
}})";
    auto analyzed = react::analyze_script(source, terms, {}, {4096, 50, 5, 64});
    require(analyzed.errors.empty(), "real script declaration validation");
    analyzed.declaration.id = 1;
    analyzed.declaration.author = 2;
    react::register_binding(vm, "balance", bindings::balance_binding, &balance);
    react::register_binding(vm, "pay", bindings::payment_binding, &payment);
    react::register_binding(vm, "issue", bindings::issuance_binding, &issuance);
    const auto loaded = react::load_script(vm, analyzed.compiled, analyzed.declaration);
    require(loaded.status == react::RunOutcome::Status::Ok, loaded.error.c_str());
  }
  react::RunOutcome run(TermId trigger, std::vector<EventField> args, Handle self = {},
                        Step step = 10) {
    effects = {};
    react::Invocation invocation;
    invocation.self = {self ? self : a, 1};
    invocation.step = step;
    invocation.payload = std::move(args);
    invocation.state = &state;
    invocation.random = &rng;
    invocation.effects = &effects;
    invocation.direct_body = react::BodyEndpoints{a, b, {}, 2, {}};
    return react::run_handler(vm, {1, terms.rows[trigger].name}, invocation);
  }
  void ok(TermId trigger, std::vector<EventField> args, Handle self = {}, Step step = 10) {
    const auto outcome = run(trigger, std::move(args), self, step);
    require(outcome.status == react::RunOutcome::Status::Ok, outcome.error.c_str());
  }
  GrantId grant(perm::Scope scope, std::int64_t cap, bool architect = false) {
    const auto holder = perm::Holder::script(1, a);
    const perm::GrantRequest request{2, holder, scope, 1, cap, 100, 1000};
    const perm::ScopeControl control{scope, 2, perm::all_rights, 0, architect};
    const auto result = perm::create_grant(grants, players, request, control, {holder, 2, true}, 0);
    require(bool(result), "ordinary capped script grant");
    return result.grant;
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
  f.ok(3, {{"expected", Value(100.)}, {"explicit", Value(100.)}});
  f.ok(1, pay_args(20, false));
  require(perm::balance(f.ledger, 2) == 100 && perm::balance(f.ledger, 3) == 0 &&
              f.effects.ordered.empty(),
          "ownership and immediate body identity do not authorize script spending");
  const auto grant = f.grant(perm::Scope::account(2), 40);
  f.ok(1, pay_args(34, true));
  require(perm::balance(f.ledger, 2) == 66 && perm::balance(f.ledger, 3) == 34 &&
              f.grants.rows[grant].spent == 34 && f.effects.changed,
          "one actual capped player wallet transfer");
  require(f.effects.ordered.size() == 1, "successful transfer records its actual effect");
  record::Journal journal(8, {});
  record::Event handler;
  handler.kind = record::Kind::Handler;
  handler.actor = record::Actor::script(1);
  const auto origin = journal.append(handler);
  step::WorkQueues queues;
  step::TimerWheel timers;
  react::publish_effects(std::move(f.effects), origin, 10, queues, timers, journal, 1000);
  const auto logs = journal.take();
  require(logs.size() == 2 && logs[1]["cause"] == origin.cause && logs[1]["name"] == "payment" &&
              logs[1]["from"] == "p:2" && logs[1]["to"] == "p:3",
          "real financial effect keeps handler origin");
  f.ok(1, pay_args(10, false));
  f.ok(1, pay_args(1, false), f.b);
  require(perm::balance(f.ledger, 3) == 34 && f.grants.rows[grant].spent == 34,
          "cap failure and another script installation preserve both wallets and counters");
  perm::revoke(f.grants, f.players, grant);
  f.ok(1, pay_args(1, false));
  require(perm::balance(f.ledger, 2) == 66, "revocation applies to next invocation");
  const auto larger = f.grant(perm::Scope::account(2), 1000);
  f.ok(1, pay_args(100, false));
  require(f.grants.rows[larger].spent == 0 && perm::balance(f.ledger, 2) == 66,
          "insufficient wallet funds do not consume a grant");
  for (double amount : {-1., .5, std::numeric_limits<double>::infinity()})
    require(f.run(1, pay_args(amount, false)).status == react::RunOutcome::Status::Error,
            "financial amounts are finite whole units");
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
  require(perm::balance(f.ledger, 3) == 21 && f.grants.rows[grant].spent == 21,
          "issuance requires a capped ordinary authority grant");
  f.ok(2, args(20, true), {}, 110);
  require(perm::balance(f.ledger, 3) == 41 && f.grants.rows[grant].spent == 20,
          "issuance period reset is applied once");
  f.ok(2, args(1, false), {}, 1000);
  require(perm::balance(f.ledger, 3) == 41, "expired issuance cannot mint");
  f.objects.owner[f.a.slot()] = 3;
  f.ok(3, {{"expected", Value(41.)}, {"explicit", Value(100.)}});
  for (const auto &wallet : {std::string("g:1"), to_string(f.a), std::string("p:999")})
    require(f.run(4, {{"wallet", wallet}}).status == react::RunOutcome::Status::Error,
            "wallet API accepts existing player identities only");
}
} // namespace
int main() {
  script_spending();
  issuance_and_wallet_identity();
  std::cout << "real Lua player wallet and grant checks passed\n";
}
