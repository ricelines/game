#include "structural_fixture.hpp"

using namespace amber;
using namespace amber::react;
using test_runtime::require;
using test_structural::Fixture;
namespace {
void ordinary_creation_bookkeeping_and_partial_writes() {
  Fixture x;
  x.f.terms.rows[7].inputs = {{"mode", InputType::String, 0, true}};
  const auto script = x.add(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,_,args)
      self.state.attempts=(self.state.attempts or 0)+1
      local result=self:act('engine.create',nil,{model='m:1',position=vector.create(3,0,0)})
      if not result.ok then self.state.denied=true;return end
      assert(result.created[1]~=nil)
      self.state.created_mass=self:measure_instance(result.created[1]).mass_kg
      self.state.created=result.created[1]
      self.state.tokens-=4
      self:set('novel.heat',self:get('novel.heat')-3)
      if args.mode=='error' then error('after completed construction') end
    end}})");
  const auto count = x.f.objects.model.size();
  auto run = [&](nlohmann::json args = nlohmann::json::object()) {
    return bindings::perform_action(x.actions, {PlayerId{1}, 7, {}, Value{}}, std::move(args), {});
  };
  auto denied = run();
  require(x.f.objects.model.size() == count &&
              std::get<double>(x.f.states.at({x.f.a, script}).at(std::string("tokens"))) == 10 &&
              read_state(x.f.properties, x.f.processes, x.f.a, 1, 0, 20).value.numeric() == 10,
          "failed Build returns to Lua without spending its stock or allocating objects");
  x.allow_build();
  auto made = run();
  const auto objects = x.created(made);
  if (objects.size() != 2)
    throw std::runtime_error(made.reply.dump());
  require(objects.size() == 2 && x.f.objects.owner[objects[0].slot()] == 1 &&
              space::bounds(x.world, identity(x.f.objects), objects[0]).has_value() &&
              space::bounds(x.world, identity(x.f.objects), objects[1]).has_value() &&
              x.f.objects.parent[objects[1].slot()] == objects[0],
          "checked create returns the actual root and ordinary attached part handles");
  require(std::get<Handle>(x.f.states.at({x.f.a, script}).at(std::string("created"))) ==
                  objects[0] &&
              std::get<double>(x.f.states.at({x.f.a, script}).at(std::string("tokens"))) == 6 &&
              read_state(x.f.properties, x.f.processes, x.f.a, 1, 0, 20).value.numeric() == 7,
          "plain Lua stock and charge bookkeeping follow the completed action");
  require(std::get<double>(x.f.states.at({x.f.a, script}).at(std::string("created_mass"))) == 1016,
          "returned handles permit actual readonly geometry queries before the handler ends");
  auto partial = run({{"mode", "error"}});
  require(partial.reply.contains("script_errors") && x.created(partial).size() == 2 &&
              std::get<double>(x.f.states.at({x.f.a, script}).at(std::string("tokens"))) == 2 &&
              read_state(x.f.properties, x.f.processes, x.f.a, 1, 0, 20).value.numeric() == 4 &&
              identity(x.f.objects).valid(x.created(partial)[0]),
          "later handler failure retains real construction and prior ordinary writes");
  auto logs = x.journal.take();
  bool child = false;
  for (const auto &event : logs)
    if (event.at("kind") == "action" && event.at("name") == "engine.create") {
      require(event.at("actor") == "s:" + std::to_string(script) && !event.at("agent").get<bool>(),
              "script construction action never masquerades as the player");
      child = true;
    }
  require(child, "nested native action has its actual S7 record");
}
void native_reconfiguration_retains_identity_and_real_geometry() {
  Fixture x;
  x.add("return {kind='model',on={['novel.touch']=function(self)end}}");
  x.allow_build();
  auto initial = x.act("engine.create", {}, {{"model", "m:1"}, {"position", {1, 0, 0}}});
  auto objects = x.created(initial);
  require(objects.size() == 2, "initial assembly created");
  const auto root = objects[0], leaf = objects[1];
  x.f.state(root, 1, 37);
  x.variants();
  auto denied = x.act("engine.reconfigure", root, {{"model", "m:1"}, {"params", 1}});
  require(denied.reply.contains("denied") && x.f.objects.params[root.slot()] == 0,
          "owning a script installation does not grant Edit on a separate created item");
  x.grant(root, perm::bit(perm::Verb::Edit));
  x.grant(leaf, perm::bit(perm::Verb::Edit));
  auto edited = x.act("engine.reconfigure", root, {{"model", "m:1"}, {"params", 1}});
  require(edited.reply.at("effect") == "applied" && x.f.objects.params[root.slot()] == 1 &&
              identity(x.f.objects).valid(leaf) &&
              read_state(x.f.properties, x.f.processes, root, 1, 0, 20).value.numeric() == 37 &&
              lifecycle::collect(x.parts, identity(x.f.objects), root).size() == 3,
          "ordinary Edit changes geometry/topology while retaining matching handles and state");
  require(space::bounds(x.world, identity(x.f.objects), root)->max.x > 1.9f,
          "reconfiguration updates actual native shape instead of metadata alone");
  auto remove = x.act("engine.destroy", root);
  require(remove.reply.contains("denied") && identity(x.f.objects).valid(root),
          "Edit authority never implies Destroy");
  x.grant(root, perm::bit(perm::Verb::Destroy));
  remove = x.act("engine.destroy", root);
  require(remove.reply.at("effect") == "applied" && !identity(x.f.objects).valid(root) &&
              !identity(x.f.objects).valid(leaf),
          "ordinary Destroy retires actual assembly geometry and identities");
}
void hidden_reconfiguration_stays_hidden_and_destroy_needs_no_reach() {
  Fixture x;
  x.add("return {kind='model',on={['novel.touch']=function(self)end}}");
  x.allow_build();
  const auto initial = x.act("engine.create", {}, {{"model", "m:1"}, {"position", {1, 0, 0}}});
  const auto objects = x.created(initial);
  const auto root = objects[0];
  x.variants();
  for (auto h : objects)
    x.grant(h, perm::bit(perm::Verb::Edit));
  std::vector<space::RelationChange> changes;
  require(space::custody(x.world, identity(x.f.objects), root, x.f.a, parents(x.f.objects),
                         space_flags(x.f.objects), x.physical, changes,
                         std::span(objects).subspan(1))
                  .status == space::Status::Ok,
          "fixture stores assembly in hidden inventory");
  x.f.objects.preset[root.slot()] |= perm::bit(perm::Verb::Edit);
  // Reconfiguration is an ordinary object edit; direct native entry uses the same
  // ScriptHolder checks while hidden goods deliberately have no query bounds.
  drive::NativeCall call{{ScriptHolder{x.script, x.f.a}, 0, root, Value{}},
                         {{"model", "m:1"}, {"params", 1}},
                         {},
                         0,
                         20,
                         {},
                         {}};
  auto edited = bindings::replace_instance(&x.replacement, call);
  require(edited.changed && !space::bounds(x.world, identity(x.f.objects), root),
          "rebuilding hidden inventory does not make its root or children query visible");
  for (auto h : lifecycle::collect(x.parts, identity(x.f.objects), root))
    require(!space::bounds(x.world, identity(x.f.objects), h),
            "hidden child remains absent from scene");
  x.grant(root, perm::bit(perm::Verb::Destroy));
  auto removed = x.act("engine.destroy", root);
  require(removed.reply.at("ok") && !identity(x.f.objects).valid(root),
          "administrative Destroy works on authorized hidden goods without invented reach");
}
void self_retirement_stops_the_suspended_handler() {
  Fixture x;
  const auto script = x.add(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self)
      self.state.before=true
      assert(self:act('engine.destroy',self.handle).ok)
      self.state.after=true
    end}})");
  x.grant(x.f.a, perm::bit(perm::Verb::Destroy));
  const auto result = bindings::perform_action(x.actions, {PlayerId{1}, 7, {}, Value{}}, {}, {});
  require(!identity(x.f.objects).valid(x.f.a) && !x.f.states.contains({x.f.a, script}) &&
              result.reply.at("effect") == "applied" && result.reply.contains("script_errors"),
          "self destruction remains applied and cannot resume through a retired state row");
}
} // namespace
int main() {
  try {
    ordinary_creation_bookkeeping_and_partial_writes();
    native_reconfiguration_retains_identity_and_real_geometry();
    hidden_reconfiguration_stays_hidden_and_destroy_needs_no_reach();
    self_retirement_stops_the_suspended_handler();
    std::cout << "Ordinary structural actions and actual lifecycle checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
