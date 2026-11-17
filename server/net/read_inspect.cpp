#include "catalog/catalog.hpp"
#include "settings.hpp"
#include "step/step.hpp"
#include "view_private.hpp"
#include <numbers>

namespace amber::net {
namespace {
std::uint32_t identifier(std::string_view value) {
  std::size_t consumed = 0;
  const auto result = std::stoul(std::string(value), &consumed);
  if (!result || consumed != value.size() || result > UINT32_MAX)
    throw std::invalid_argument("Invalid catalog identifier");
  return static_cast<std::uint32_t>(result);
}
Json script_record(const catalog::Scripts &scripts, ScriptId id) {
  const auto &script = scripts.rows.at(id);
  if (!script.id)
    throw std::invalid_argument("Unknown script");
  Json result = {{"script", "s:" + std::to_string(id)},
                 {"author", "p:" + std::to_string(script.author)},
                 {"created_s", script.created_s},
                 {"hash", script.artifact},
                 {"declaration", catalog::declaration_json(script.declaration)}};
  result["source"] = script.source;
  result["source_available"] = true;
  return result;
}
Json model_record(const catalog::Models &models, const catalog::ParameterBlocks &parameters,
                  const catalog::Assets &assets, const catalog::Scripts &scripts,
                  const catalog::ModelTexts &model_text, const react::ValueRead &values,
                  ModelId id) {
  const auto &model = models.rows.at(id);
  if (!model.id)
    throw std::invalid_argument("Unknown model");
  const auto &asset = assets.rows.at(model.asset);
  Json result = {{"model", "m:" + std::to_string(id)},
                 {"author", "p:" + std::to_string(model.author)},
                 {"created_s", model.created_s},
                 {"material", values.terms.rows.at(model.material).name},
                 {"asset", model.asset},
                 {"source_hash", model.source_hash},
                 {"canonical_params", model.canonical_params},
                 {"manifest", asset.manifest},
                 {"variants", Json::array()}};
  if (const auto text = model_text.rows.find(id); text != model_text.rows.end())
    result["says"] = view_detail::text(text->second);
  result["source"] = asset.source;
  result["source_available"] = true;
  if (model.script)
    result["script"] = script_record(scripts, model.script);
  for (const auto &variant : parameters.rows)
    if (variant.id && variant.model == id &&
        assets.rows.at(variant.asset).source_hash == model.source_hash)
      result["variants"].push_back({{"params", variant.id},
                                    {"asset", variant.asset},
                                    {"canonical_params", variant.canonical_params}});
  return result;
}
Json scope_record(perm::Scope scope) {
  switch (scope.kind) {
  case perm::ScopeKind::Object:
    return {{"object", to_string(Handle{scope.subject})}};
  case perm::ScopeKind::Owned:
    return {{"owned", "p:" + std::to_string(scope.owner)}};
  case perm::ScopeKind::AllLand:
    return {{"all_land", true}};
  case perm::ScopeKind::OwnedOnPlot:
    return {{"owner", "p:" + std::to_string(scope.owner)}, {"plot", scope.subject}};
  case perm::ScopeKind::Account:
    return {{"account", perm::account_name(scope.account())}};
  case perm::ScopeKind::Avatar:
    return {{"avatar", to_string(Handle{scope.subject})}};
  case perm::ScopeKind::Plot:
    return {{"plot", scope.subject}};
  }
  return nullptr;
}
Json holder_record(perm::Holder holder) {
  if (holder.kind == perm::HolderKind::Player)
    return "p:" + std::to_string(holder.id);
  return {{"script", "s:" + std::to_string(holder.id)},
          {"on", holder.installed_on ? to_string(holder.installed_on) : "world"}};
}
Json grant_record(const perm::Grants &grants, const perm::Grant &grant, Step now, double hz) {
  return {
      {"id", "g:" + std::to_string(grant.id)},
      {"issuer", holder_record(!grant.issuer && grant.parent ? grants.rows[grant.parent].holder
                                                             : perm::Holder::player(grant.issuer))},
      {"holder", holder_record(grant.holder)},
      {"scope", scope_record(grant.scope)},
      {"rights", grant.verbs},
      {"parent", grant.parent ? Json("g:" + std::to_string(grant.parent)) : Json(nullptr)},
      {"cap", grant.cap},
      {"remaining", perm::allowance(grants, grant.id, now)},
      {"period_s", grant.period_steps / hz},
      {"expires_s", grant.expires_step / hz},
      {"live", grant.live}};
}
Json state_record(const react::ScriptState &state) {
  std::vector<react::StateKey> keys;
  keys.reserve(state.values.size());
  for (const auto &[key, value] : state.values) {
    (void)value;
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end());
  Json result = Json::array();
  constexpr std::array types{"number", "boolean", "string", "handle"};
  for (const auto &key : keys) {
    const auto &atom = state.values.at(key);
    Json row;
    row["key"] = std::visit([](const auto &value) -> Json { return value; }, key);
    row["type"] = types[atom.index()];
    row["value"] = std::visit(
        [](const auto &value) -> Json {
          if constexpr (std::is_same_v<std::decay_t<decltype(value)>, Handle>)
            return to_string(value);
          else
            return value;
        },
        atom);
    result.push_back(std::move(row));
  }
  return result;
}
Json object_record(const amber::catalog::State &catalog_state,
                   const amber::react::State &react_state, const amber::ObjectState &objects_state,
                   const amber::perm::State &perm_state, const amber::step::Clock &clock,
                   Handle object, const net::Invocation &call) {
  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &models = catalog_state.models;

  auto &scripts = catalog_state.scripts;

  auto &instances = react_state.installations;

  auto &inspection_terms = react_state.inspection_terms;

  auto transforms = amber::transforms(std::as_const(objects_state.objects));
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto parents = amber::parents(std::as_const(objects_state.objects));
  auto attribution = amber::attribution(std::as_const(objects_state.objects));
  auto &relations = react_state.relations;
  auto &limits = perm_state.owner_limits;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;
  auto &states = react_state.states;
  auto &triggers = react_state.triggers;

  auto result = view_detail::description(
      view_detail::describe(catalog_state, react_state, values, object, now, hz, inspection_terms));
  const auto identity = values.identity;
  result["owner"] = "p:" + std::to_string(authority.owner[object.slot()]);
  result["maker"] = "p:" + std::to_string(attribution.maker[object.slot()]);
  result["created_s"] = attribution.created_step[object.slot()] / hz;
  result["position"] = view_detail::vector(transforms.position[object.slot()]);
  const auto q = transforms.rotation[object.slot()];
  result["rotation"] = {q.x, q.y, q.z, q.w};
  const auto parent = parents.parent[object.slot()];
  result["parent"] = parent ? Json(to_string(parent)) : Json(nullptr);
  result["plot"] = authority.plot[object.slot()];
  result["preset"] = authority.preset[object.slot()];
  result["flags"] = identity.flags[object.slot()];
  result["values"] = Json::object();
  for (const auto &term : values.terms.rows)
    if (term.id && (term.kind == react::Kind::Quantity || term.kind == react::Kind::Flag)) {
      const auto value = react::read(values, object, term.id, now, hz);
      if (value.present)
        result["values"][term.name] = view_detail::scalar(value.value);
    }
  result["relations"] = Json::object();
  for (const auto &[term, relation] : relations.columns) {
    Json incoming = Json::array(), outgoing = Json::array();
    if (const auto *rows = relation->out.get(object.slot()))
      for (const auto h : *rows)
        if (identity.valid(h))
          outgoing.push_back(to_string(h));
    if (const auto *rows = relation->in.get(object.slot()))
      for (const auto h : *rows)
        if (identity.valid(h))
          incoming.push_back(to_string(h));
    if (!incoming.empty() || !outgoing.empty())
      result["relations"][values.terms.rows.at(term).name] = {{"in", incoming}, {"out", outgoing}};
  }
  const auto target = perm::target(identity, authority, limits, perm_state.plots, object);
  result["rights"] = Json::object();
  constexpr const char *names[]{"use", "take", "move", "edit", "copy", "transfer", "destroy"};
  for (unsigned index = 0; index < 7; ++index) {
    const auto decision = perm::check(perm::Holder::player(call.principal.player),
                                      static_cast<perm::Verb>(index), target, players, grants, now);
    result["rights"][names[index]] = {{"allowed", bool(decision)},
                                      {"reason", perm::reason_text(decision.reason)}};
  }
  result["scripts"] = Json::array();
  for (const auto id : react::installed_scripts(models, instances, identity, object))
    result["scripts"].push_back(script_record(scripts, id));
  if (target.owner == call.principal.player ||
      perm::check(perm::Holder::player(call.principal.player), perm::Verb::Edit, target, players,
                  grants, now)) {
    auto installed_ids = react::installed_scripts(models, instances, identity, object);
    const auto applicable = react::rule_scripts(triggers, values, object, now, hz);
    installed_ids.insert(installed_ids.end(), applicable.begin(), applicable.end());
    std::sort(installed_ids.begin(), installed_ids.end());
    installed_ids.erase(std::unique(installed_ids.begin(), installed_ids.end()),
                        installed_ids.end());
    result["script_states"] = Json::array();
    for (auto script : installed_ids) {
      const auto found = states.find({object, script});
      result["script_states"].push_back(
          {{"script", "s:" + std::to_string(script)},
           {"state", found == states.end() ? Json::array() : state_record(found->second)}});
    }
  }
  return result;
}
} // namespace
net::Reply read_inspect(const amber::catalog::State &catalog_state,
                        const amber::react::State &react_state, observe::Hearing &hearing,
                        const amber::ObjectState &objects_state,
                        const amber::perm::State &perm_state, const amber::step::Clock &clock,
                        const amber::Settings &settings, const net::Invocation &call) {
  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;
  auto &scripts = catalog_state.scripts;
  auto &model_text = catalog_state.model_text;

  auto &players = perm_state.players;
  auto &grants = perm_state.grants;
  auto &ledger = perm_state.ledger;

  try {
    const auto name = call.request.fields.at("target").get<std::string>();
    Json result;
    if (name.starts_with("m:"))
      result = model_record(models, parameters, assets, scripts, model_text, values,
                            identifier(name.substr(2)));
    else if (name.starts_with("s:"))
      result = script_record(scripts, identifier(name.substr(2)));
    else if (name == "inventory") {
      result = {{"inventory", Json::array()}, {"mailbox", Json::array()}};
      const auto &player = players.rows.at(call.principal.player);
      for (const auto h : player.inventory)
        if (values.identity.valid(h))
          result["inventory"].push_back(
              object_record(catalog_state, react_state, objects_state, perm_state, clock, h, call));
      for (const auto h : player.mailbox)
        if (values.identity.valid(h))
          result["mailbox"].push_back(
              object_record(catalog_state, react_state, objects_state, perm_state, clock, h, call));
      result["balance"] = perm::balance(ledger, perm::Account::player(call.principal.player));
    } else if (name == "grants") {
      result = {{"held", Json::array()}, {"given", Json::array()}};
      const auto &player = players.rows.at(call.principal.player);
      for (const auto id : player.grants_held)
        if (grants.rows.at(id).live)
          result["held"].push_back(grant_record(grants, grants.rows[id], now, hz));
      for (const auto id : player.grants_given)
        if (grants.rows.at(id).live)
          result["given"].push_back(grant_record(grants, grants.rows[id], now, hz));
    } else if (name == "land" || name == "plot" || name.starts_with("plot:")) {
      result = view_detail::inspect_land(catalog_state, react_state, objects_state, perm_state,
                                         clock, settings, call);
    } else {
      const auto object = view_detail::target(values.identity, call);
      result =
          object_record(catalog_state, react_state, objects_state, perm_state, clock, object, call);
      if (name == "self")
        result["balance"] = perm::balance(ledger, perm::Account::player(call.principal.player));
    }
    result["ok"] = true;
    append_heard(
        result, hearing, call.session,
        call.request.fields.value("events_limit", settings.perception.events_per_response));
    return net::Reply::result(std::move(result));
  } catch (const std::exception &error) {
    return view_detail::error(error.what());
  }
}
} // namespace amber::net
