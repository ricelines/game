#include "common.hpp"

namespace amber::bindings::viewing {
namespace {
std::uint32_t identifier(std::string_view value) {
  std::size_t consumed = 0;
  const auto result = std::stoul(std::string(value), &consumed);
  if (!result || consumed != value.size() || result > UINT32_MAX)
    throw std::invalid_argument("Invalid catalog identifier");
  return static_cast<std::uint32_t>(result);
}
Json script_record(const InspectBinding &b, ScriptId id) {
  const auto &script = b.catalog.scripts.rows.at(id);
  if (!script.id)
    throw std::invalid_argument("Unknown script");
  Json result = {{"script", "s:" + std::to_string(id)},
                 {"author", "p:" + std::to_string(script.author)},
                 {"created_s", script.created_s},
                 {"hash", script.artifact},
                 {"declaration", catalog::declaration_json(script.declaration)}};
  const auto source = b.sources.scripts.find(id);
  result["source"] = source == b.sources.scripts.end() ? Json(nullptr) : Json(source->second);
  result["source_available"] = source != b.sources.scripts.end();
  return result;
}
Json model_record(const InspectBinding &b, ModelId id) {
  const auto &model = b.catalog.models.rows.at(id);
  if (!model.id)
    throw std::invalid_argument("Unknown model");
  const auto &asset = b.catalog.assets.rows.at(model.asset);
  Json result = {{"model", "m:" + std::to_string(id)},
                 {"author", "p:" + std::to_string(model.author)},
                 {"created_s", model.created_s},
                 {"material", b.catalog.values.terms.rows.at(model.material).name},
                 {"asset", model.asset},
                 {"source_hash", model.source_hash},
                 {"canonical_params", model.canonical_params},
                 {"manifest", asset.manifest},
                 {"variants", Json::array()}};
  if (const auto text = b.catalog.model_text.rows.find(id); text != b.catalog.model_text.rows.end())
    result["says"] = detail::text(text->second);
  const auto source = b.sources.assets.find(model.asset);
  result["source"] = source == b.sources.assets.end() ? Json(nullptr) : Json(source->second);
  result["source_available"] = source != b.sources.assets.end();
  if (model.script)
    result["script"] = script_record(b, model.script);
  for (const auto &variant : b.catalog.parameters.rows)
    if (variant.id && variant.model == id &&
        b.catalog.assets.rows.at(variant.asset).source_hash == model.source_hash)
      result["variants"].push_back({{"params", variant.id},
                                    {"asset", variant.asset},
                                    {"canonical_params", variant.canonical_params}});
  return result;
}
Json scope_record(perm::Scope scope) {
  switch (scope.kind) {
  case perm::ScopeKind::Object:
    return {{"object", to_string(Handle{scope.subject})}};
  case perm::ScopeKind::OwnedOnPlot:
    return {{"owner", "p:" + std::to_string(scope.owner)}, {"plot", scope.subject}};
  case perm::ScopeKind::Account:
    return {{"account", perm::account_name(scope.account())}};
  case perm::ScopeKind::Avatar:
    return {{"avatar", to_string(Handle{scope.subject})}};
  case perm::ScopeKind::Plot:
    return {{"plot", scope.subject}};
  case perm::ScopeKind::Issuance:
    return {{"issuance", true}};
  }
  return nullptr;
}
Json grant_record(const perm::Grants &grants, const perm::Grant &grant, Step now, double hz) {
  Json holder;
  if (grant.holder.kind == perm::HolderKind::Player)
    holder = "p:" + std::to_string(grant.holder.id);
  else
    holder = {{"script", "s:" + std::to_string(grant.holder.id)},
              {"on", to_string(grant.holder.installed_on)}};
  return {{"id", grant.id},
          {"issuer", "p:" + std::to_string(grant.issuer)},
          {"holder", holder},
          {"scope", scope_record(grant.scope)},
          {"rights", grant.verbs},
          {"parent", grant.parent},
          {"cap", grant.cap},
          {"remaining", perm::allowance(grants, grant.id, now)},
          {"period_s", grant.period_steps / hz},
          {"expires_s", grant.expires_step / hz},
          {"live", grant.live}};
}
Json plot_record(const perm::Plot &plot) {
  return {{"id", plot.id},
          {"owner", "p:" + std::to_string(plot.owner)},
          {"group", plot.group},
          {"bounds",
           {{"min", {plot.bounds[0], plot.bounds[1]}}, {"max", {plot.bounds[2], plot.bounds[3]}}}},
          {"preset", plot.preset}};
}
Json state_record(const react::ScriptState &state) {
  std::vector<react::StateKey> keys;
  keys.reserve(state.size());
  for (const auto &[key, value] : state) {
    (void)value;
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end());
  Json result = Json::array();
  constexpr std::array types{"number", "boolean", "string", "handle"};
  for (const auto &key : keys) {
    const auto &atom = state.at(key);
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
Json object_record(InspectBinding &b, Handle object, const control::Invocation &call) {
  auto result = detail::description(detail::describe(b.catalog, object, b.now, b.hz));
  const auto identity = b.catalog.values.identity;
  result["owner"] = "p:" + std::to_string(b.authority.owner[object.slot()]);
  result["maker"] = "p:" + std::to_string(b.attribution.maker[object.slot()]);
  result["created_s"] = b.attribution.created_step[object.slot()] / b.hz;
  result["position"] = detail::vector(b.transforms.position[object.slot()]);
  const auto q = b.transforms.rotation[object.slot()];
  result["rotation"] = {q.x, q.y, q.z, q.w};
  const auto parent = b.parents.parent[object.slot()];
  result["parent"] = parent ? Json(to_string(parent)) : Json(nullptr);
  result["plot"] = b.authority.plot[object.slot()];
  result["preset"] = b.authority.preset[object.slot()];
  result["flags"] = identity.flags[object.slot()];
  result["values"] = Json::object();
  for (const auto &term : b.catalog.values.terms.rows)
    if (term.id && (term.kind == react::Kind::Quantity || term.kind == react::Kind::Flag)) {
      const auto value = react::read(b.catalog.values, object, term.id, b.now, b.hz);
      if (value.present)
        result["values"][term.name] = detail::scalar(value.value);
    }
  result["relations"] = Json::object();
  for (const auto &[term, relation] : b.relations.columns) {
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
      result["relations"][b.catalog.values.terms.rows.at(term).name] = {{"in", incoming},
                                                                        {"out", outgoing}};
  }
  const auto target = perm::target(identity, b.authority, b.limits, object);
  result["rights"] = Json::object();
  constexpr const char *names[]{"use", "take", "move", "edit", "copy", "transfer", "destroy"};
  for (unsigned index = 0; index < 7; ++index) {
    const auto decision =
        perm::check(perm::Holder::player(call.principal.player), static_cast<perm::Verb>(index),
                    target, b.players, b.grants, b.now);
    result["rights"][names[index]] = {{"allowed", bool(decision)},
                                      {"reason", perm::reason_text(decision.reason)}};
  }
  result["scripts"] = Json::array();
  for (const auto id :
       install::installed_scripts(b.catalog.models, b.catalog.instances, identity, object))
    result["scripts"].push_back(script_record(b, id));
  if (target.owner == call.principal.player ||
      perm::check(perm::Holder::player(call.principal.player), perm::Verb::Edit, target, b.players,
                  b.grants, b.now)) {
    auto scripts =
        install::installed_scripts(b.catalog.models, b.catalog.instances, identity, object);
    const auto rules = install::rule_scripts(b.rules, b.catalog.values, object, b.now, b.hz);
    scripts.insert(scripts.end(), rules.begin(), rules.end());
    std::sort(scripts.begin(), scripts.end());
    scripts.erase(std::unique(scripts.begin(), scripts.end()), scripts.end());
    result["script_states"] = Json::array();
    for (auto script : scripts) {
      const auto found = b.states.find({object, script});
      result["script_states"].push_back(
          {{"script", "s:" + std::to_string(script)},
           {"state", found == b.states.end() ? Json::array() : state_record(found->second)}});
    }
  }
  b.sessions.rows[call.session].acquired.insert(object.bits);
  return result;
}
} // namespace
control::Reply inspect_handler(void *local, const control::Invocation &call) {
  auto &b = *static_cast<InspectBinding *>(local);
  try {
    const auto name = call.request.fields.at("target").get<std::string>();
    Json result;
    if (name.starts_with("m:"))
      result = model_record(b, identifier(name.substr(2)));
    else if (name.starts_with("s:"))
      result = script_record(b, identifier(name.substr(2)));
    else if (name == "inventory") {
      result = {{"inventory", Json::array()}, {"mailbox", Json::array()}};
      const auto &player = b.players.rows.at(call.principal.player);
      for (const auto h : player.inventory)
        if (b.catalog.values.identity.valid(h))
          result["inventory"].push_back(object_record(b, h, call));
      for (const auto h : player.mailbox)
        if (b.catalog.values.identity.valid(h))
          result["mailbox"].push_back(object_record(b, h, call));
      result["balance"] = perm::balance(b.ledger, perm::Account::player(call.principal.player));
    } else if (name == "grants") {
      result = {{"held", Json::array()}, {"given", Json::array()}};
      const auto &player = b.players.rows.at(call.principal.player);
      for (const auto id : player.grants_held)
        if (b.grants.rows.at(id).live)
          result["held"].push_back(grant_record(b.grants, b.grants.rows[id], b.now, b.hz));
      for (const auto id : player.grants_given)
        if (b.grants.rows.at(id).live)
          result["given"].push_back(grant_record(b.grants, b.grants.rows[id], b.now, b.hz));
    } else if (name == "plots") {
      result = {{"plots", Json::array()}};
      for (const auto &plot : b.plots.rows)
        if (plot.id && plot.owner == call.principal.player)
          result["plots"].push_back(plot_record(plot));
    } else if (name.starts_with("plot:")) {
      const auto id = identifier(std::string_view(name).substr(5));
      if (id >= b.plots.rows.size() || b.plots.rows[id].id != id)
        throw std::invalid_argument("Unknown plot");
      result = {{"plot", plot_record(b.plots.rows[id])}};
    } else if (name == "plot") {
      if (!b.catalog.values.identity.valid(call.principal.body))
        return detail::error("Player has no live body");
      const auto p = b.transforms.position[call.principal.body.slot()];
      const perm::Plot *found = nullptr;
      for (const auto &plot : b.plots.rows)
        if (plot.id && p.x >= plot.bounds[0] && p.x <= plot.bounds[2] && p.y >= plot.bounds[1] &&
            p.y <= plot.bounds[3]) {
          found = &plot;
          break;
        }
      if (!found)
        result = {{"plot", nullptr}};
      else
        result = {{"plot", plot_record(*found)}};
    } else {
      const auto object = detail::target(b.catalog, call);
      result = object_record(b, object, call);
      if (name == "self")
        result["balance"] = perm::balance(b.ledger, perm::Account::player(call.principal.player));
    }
    result["ok"] = true;
    append_heard(result, b.hearing, call.session,
                 call.request.fields.value("events_limit", b.events_per_response));
    return control::Reply::result(std::move(result));
  } catch (const std::exception &error) {
    return detail::error(error.what());
  }
}
} // namespace amber::bindings::viewing
