#include "bindings/resources/resources.hpp"

#include <iostream>
#include <stdexcept>

using namespace amber;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Fixture {
  react::Terms terms;
  react::Properties properties;
  react::Processes processes;
  react::ScriptState state;
  react::DeferredEffects effects;
  const Handle object = Handle::make(1, 4);
  Fixture() {
    terms.rows.resize(4);
    for (TermId i = 1; i < terms.rows.size(); ++i) {
      terms.rows[i].id = i;
      terms.rows[i].name = "custom." + std::to_string(i);
      terms.by_name[terms.rows[i].name] = i;
    }
    terms.rows[3].kind = react::Kind::Event;
    properties.resize(3);
    processes.resize(3);
    properties[1].set(object.slot(), Value(10.0));
    processes[1].set(object.slot(), react::Process{10, 0, -1, 0, 20, 9});
    properties[2].set(object.slot(), Value(false));
    state[std::int64_t{1}] = 2.0;
  }
  bool apply(const react::ResourceExchange &exchange, std::size_t capacity = 4096) {
    std::string error;
    const auto result = react::apply_exchange(properties, processes, state, terms, exchange, 10, 20,
                                              capacity, effects, error);
    require(result == error.empty(), "exchange success matches reported validation");
    return result;
  }
};
void atomic_preflight_and_continuing_process() {
  Fixture f;
  react::ResourceExchange exchange{{f.object, 7},
                                   {{1, Value(9.5), Value(4.5)}, {2, Value(true), Value(true)}},
                                   {{std::int64_t{1}, react::StateAtom(2.0), 5.0}}};
  require(!f.apply(exchange) && f.processes[1].get(1)->revision == 9 &&
              std::get<double>(f.state.at(std::int64_t{1})) == 2 &&
              react::read_state(f.properties, f.processes, f.object, 1, 10, 20).value.numeric() ==
                  9.5,
          "later stale prior answer prevents every earlier debit and credit");
  exchange.terms[1].before = Value(false);
  require(!f.apply(exchange, 1) && f.processes[1].get(1)->revision == 9,
          "flat-state quota failure prevents all term writes");
  exchange.terms[0].after = Value(-1.0);
  require(!f.apply(exchange), "process bound failure is explicit rather than clamped");
  exchange.terms[0].after = Value(4.5);
  require(f.apply(exchange) && f.processes[1].get(1)->rate == -1 &&
              f.processes[1].get(1)->revision == 10 &&
              react::read_state(f.properties, f.processes, f.object, 1, 20, 20).value.numeric() ==
                  4 &&
              std::get<double>(f.state.at(std::int64_t{1})) == 5,
          "completed debit preserves bounded process continuation and credits stock once");
  require(!f.apply(exchange) && std::get<double>(f.state.at(std::int64_t{1})) == 5,
          "second request cannot spend the same captured balance");
  exchange.terms.clear();
  exchange.state = {{std::string("1"), {}, 8.0}};
  require(f.apply(exchange) && f.state.size() == 2 &&
              std::get<double>(f.state.at(std::string("1"))) == 8,
          "integer and string keys stay distinct");
}
void actual_lua_capture_and_memory_accounting() {
  Fixture f;
  react::Vm vm{{256, 50, 5, 64}};
  bindings::ExchangeBinding binding{f.properties, f.processes, f.terms, 64};
  react::register_binding(vm, "exchange", bindings::exchange_binding, &binding);
  const auto source = R"(return {kind='instance',sets={'custom.1','custom.2'},on={
    ['custom.3']=function(self)
      self:exchange({terms={['custom.1']=4.5,['custom.2']=true},
                     state={[1]=5,['1']=8,note='kept'}})
      assert(self.state[1]==5 and self.state['1']==8 and self.state.note=='kept')
    end}})";
  auto analyzed = react::analyze_script(source, f.terms, {}, {256, 50, 5, 64});
  require(analyzed.errors.empty(), "real exchange script declaration");
  analyzed.declaration.id = 7;
  require(react::load_script(vm, analyzed.compiled, analyzed.declaration).status ==
              react::RunOutcome::Status::Ok,
          "load actual exchange Lua");
  react::Invocation invocation;
  invocation.self = {f.object, 7};
  invocation.step = 10;
  invocation.state = &f.state;
  invocation.effects = &f.effects;
  const auto run = react::run_handler(vm, {7, "custom.3"}, invocation);
  require(run.status == react::RunOutcome::Status::Ok, run.error.c_str());
  require(f.effects.changed && f.state.size() == 3 && f.processes[1].get(1)->rate == -1 &&
              react::state_bytes(f.state) <= react::state_capacity(vm, 7),
          "real binding commits both forms and maintains VM persistent budget");
}
} // namespace
int main() {
  try {
    atomic_preflight_and_continuing_process();
    actual_lua_capture_and_memory_accounting();
    std::cout << "Generic atomic resource exchange and actual Luau binding tests passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
