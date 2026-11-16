#include "catalog/catalog.hpp"
#include "start_private.hpp"

namespace amber::step::start {
std::vector<Handle> install_objects(const Json &plan, Symbols &symbols,
                                    const amber::catalog::State &catalog_state,
                                    const react::Terms &terms, amber::ObjectState &objects_state,
                                    Step now) {
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;
  auto &objects = objects_state.objects;
  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;

  std::vector<Handle> created;
  for (const auto &row : plan.at("objects")) {
    const auto model = symbols.models.at(row.at("model").get<std::string>());
    const auto assembly = lifecycle::expand(models, parameters, assets, model);
    ObjectInit init;
    init.position = detail::vector(row.at("at"));
    init.rotation = detail::quaternion(row.at("rotation"));
    init.owner = row.at("owner").get<PlayerId>();
    init.maker = row.at("maker").get<PlayerId>();
    init.created_step = now;
    init.group = row.value("group", GroupId{});
    init.preset = detail::rights(row.value("preset", Json::array())) |
                  (detail::rights(row.value("group_rights", Json::array())) << 8);
    init.flags = 0;
    for (const auto &flag : row.at("flags")) {
      const auto name = flag.get<std::string>();
      if (name == "static")
        init.flags |= Static;
      else if (name == "dynamic")
        init.flags |= Dynamic;
      else if (name == "body")
        init.flags |= Body;
      else if (name == "scripted_motion")
        init.flags |= ScriptedMotion;
      else if (name == "no_copy")
        init.flags |= NoCopy;
      else
        throw std::invalid_argument("unknown initial object flag");
    }
    if (row.contains("material"))
      init.material = detail::term(terms, row.at("material"));
    const auto result = lifecycle::instantiate(objects, parts, declared_parts, assembly, init);
    if (!symbols.objects.emplace(row.at("key").get<std::string>(), result.root).second)
      throw std::invalid_argument("duplicate initial object symbol");
    created.insert(created.end(), result.objects.begin(), result.objects.end());
  }
  return created;
}
void install_properties(const Json &plan, const Symbols &symbols, amber::react::State &react_state,
                        Step now, double hz) {
  auto &terms = react_state.terms;
  auto &properties = react_state.properties;
  auto &processes = react_state.processes;
  auto &instances = react_state.instance_settings;
  auto &world = react_state.world_settings;

  properties.resize(terms.rows.size());
  processes.resize(terms.rows.size());
  instances.columns.resize(terms.rows.size());
  auto setting = [&](const std::string &name, const Json &value) {
    const auto id = detail::term(terms, name);
    if (terms.rows[id].source != react::Source::Setting)
      throw std::invalid_argument("initial setting names a non-setting term");
    return std::pair{id, detail::scalar(value, symbols)};
  };
  for (const auto &[name, value] : plan.at("world_settings").items()) {
    const auto [id, scalar] = setting(name, value);
    world.values[id] = scalar;
  }
  react::DeferredEffects initial;
  for (const auto &row : plan.at("objects")) {
    const auto object = symbols.objects.at(row.at("key").get<std::string>());
    for (const auto &[name, value] : row.at("state").items()) {
      const auto id = detail::term(terms, name);
      react::set_state(properties, processes, terms.rows[id], object,
                       detail::scalar(value, symbols), now, hz, initial);
    }
    for (const auto &[name, value] : row.at("settings").items()) {
      const auto [id, scalar] = setting(name, value);
      instances.columns[id].set(object.slot(), scalar);
    }
  }
}
void install_relations(const Json &plan, const Symbols &symbols, const react::Terms &terms,
                       react::AuthoredRelations &relations, react::DeferredEffects &effects) {
  for (const auto &row : plan.at("relations")) {
    const auto id = detail::term(terms, row.at("term"));
    if (terms.rows[id].kind != react::Kind::Relation)
      throw std::invalid_argument("initial edge requires a relation term");
    react::set_relation(relations[id], id, symbols.objects.at(row.at("from").get<std::string>()),
                        symbols.objects.at(row.at("to").get<std::string>()), true, effects);
  }
}
void install_script_state(const Json &plan, const Symbols &symbols, react::ScriptStates &states) {
  for (const auto &row : plan.at("script_state")) {
    const ScriptKey key{symbols.objects.at(row.at("target").get<std::string>()),
                        symbols.scripts.at(row.at("script").get<std::string>())};
    auto &state = states[key];
    for (const auto &entry : row.at("entries")) {
      react::StateKey name;
      if (entry.at("key").is_string())
        name = entry.at("key").get<std::string>();
      else
        name = entry.at("key").get<std::int64_t>();
      const auto &value = entry.at("value");
      react::StateAtom atom;
      if (value.is_string())
        atom = value.get<std::string>();
      else if (value.is_boolean())
        atom = value.get<bool>();
      else if (value.is_number())
        atom = value.get<double>();
      else if (value.is_object() && value.size() == 1 && value.contains("object"))
        atom = symbols.objects.at(value.at("object").get<std::string>());
      else
        throw std::invalid_argument("initial script state needs a flat scalar or typed handle");
      if (!state.emplace(std::move(name), std::move(atom)).second)
        throw std::invalid_argument("duplicate initial flat-state key");
    }
  }
}
void queue_bootstrap(const Json &plan, const Symbols &symbols, const react::Terms &terms,
                     step::WorkQueues &queues, std::size_t event_limit) {
  auto atom = [&](const Json &value) -> EventAtom {
    if (value.is_string())
      return value.get<std::string>();
    if (value.is_array())
      return detail::vector(value);
    return detail::scalar(value, symbols);
  };
  for (const auto &row : plan.at("bootstrap")) {
    const auto id = detail::term(terms, row.at("event"));
    if (terms.rows[id].kind != react::Kind::Event)
      throw std::invalid_argument("bootstrap needs an event term");
    EventPayload payload;
    const auto &value = row.at("payload");
    if (value.is_object() && !value.contains("object")) {
      std::vector<EventField> fields;
      for (const auto &[name, item] : value.items())
        fields.push_back({name, atom(item)});
      payload = std::move(fields);
    } else if (value.is_string())
      payload = value.get<std::string>();
    else if (value.is_array())
      payload = detail::vector(value);
    else if (!value.is_null())
      payload = detail::scalar(value, symbols);
    const auto queued = step::enqueue_event(
        queues,
        {symbols.objects.at(row.at("target").get<std::string>()), {}, id, std::move(payload), {}},
        event_limit);
    if (queued != step::PushResult::Queued)
      throw std::invalid_argument("initial event queue exceeds configured capacity");
  }
}
void record_seed(record::Journal &journal, const react::Terms &terms, const catalog::Models &models,
                 const catalog::Scripts &scripts, std::span<const Handle> initial, IdentityRead ids,
                 AuthorityRead auth, AttributionRead makers) {
  for (const auto &term : terms.rows)
    if (term.id) {
      record::Event event;
      event.kind = record::Kind::Create;
      event.actor = record::Actor::player(term.owner);
      event.name = "seed_term";
      event.detail = {{"of", "term"},
                      {"term", term.id},
                      {"seed", true},
                      {"agent", false},
                      {"maker", "p:" + std::to_string(term.owner)}};
      journal.append(std::move(event));
    }
  for (const auto &model : models.rows)
    if (model.id) {
      record::Event event;
      event.kind = record::Kind::Create;
      event.actor = record::Actor::player(model.author);
      event.name = "seed_model";
      event.detail = {{"of", "model"},
                      {"model", "m:" + std::to_string(model.id)},
                      {"maker", "p:" + std::to_string(model.author)},
                      {"agent", false},
                      {"seed", true}};
      journal.append(std::move(event));
    }
  for (const auto &script : scripts.rows)
    if (script.id) {
      record::Event event;
      event.kind = record::Kind::Create;
      event.actor = record::Actor::player(script.author);
      event.name = "seed_script";
      event.detail = {{"of", "script"},
                      {"script", "s:" + std::to_string(script.id)},
                      {"maker", "p:" + std::to_string(script.author)},
                      {"agent", false},
                      {"seed", true}};
      journal.append(std::move(event));
    }
  for (auto object : initial) {
    record::Event event;
    event.kind = record::Kind::Create;
    event.actor = record::Actor::player(makers.maker[object.slot()]);
    event.name = "seed_instance";
    event.things = {object};
    event.detail = {{"of", "instance"},
                    {"model", "m:" + std::to_string(ids.model[object.slot()])},
                    {"maker", "p:" + std::to_string(makers.maker[object.slot()])},
                    {"owner", "p:" + std::to_string(auth.owner[object.slot()])},
                    {"material", ids.material[object.slot()]},
                    {"agent", false},
                    {"seed", true}};
    journal.append(std::move(event));
  }
}
} // namespace amber::step::start
