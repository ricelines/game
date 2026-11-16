#include "action_private.hpp"
#include <stdexcept>

namespace amber::step {
NativeCompletion
native_mechanics(amber::ObjectState &objects_state, amber::react::State &react_state,
                 const amber::perm::State &perm_state, amber::space::State &space_state,
                 MechanicalCauses &mechanical_causes, PhysicalRelationTerms physical_terms,
                 float reach_m, react::MechanicAction operation, const NativeCall &call) {
  auto &space = space_state.space;

  auto &plots = perm_state.plots;

  auto identity = amber::identity(std::as_const(objects_state.objects));

  NativeCompletion out;
  switch (operation) {
  case react::MechanicAction::Grasp: {
    out = grasp_action(objects_state, space_state, physical_terms, reach_m, mechanical_causes,
                       plots, call);
    break;
  }
  case react::MechanicAction::Release: {
    out = release_action(objects_state, space_state, physical_terms, perm_state, mechanical_causes,
                         call);
    break;
  }
  case react::MechanicAction::Attach: {
    out = attach_action(objects_state, space_state, physical_terms, perm_state, reach_m,
                        mechanical_causes, call);
    break;
  }
  case react::MechanicAction::Impulse: {
    out = impulse_action(space, identity, mechanical_causes, call);
    break;
  }
  case react::MechanicAction::Transfer: {
    out = transfer_action(react_state, objects_state, perm_state, call);
    break;
  }
  }
  return out;
}
NativeCompletion native_construction(amber::ObjectState &objects_state,
                                     amber::catalog::State &catalog_state,
                                     amber::react::State &react_state,
                                     amber::perm::State &perm_state, const GeometryAssets &geometry,
                                     amber::space::State &space_state,
                                     PhysicsDefaults physics_defaults,
                                     react::ConstructionAction operation, const NativeCall &call) {
  const auto &physics = catalog_state.mechanical.values.at("physics");
  const PhysicsRoles physics_roles{
      physics.at("density").get<TermId>(), physics.at("transmission").get<TermId>(),
      physics.value("friction", TermId{}), physics.value("restitution", TermId{})};
  const auto &relations = catalog_state.mechanical.values.at("relations");
  const std::array<TermId, 4> relation_terms{
      relations.at("contains").get<TermId>(), relations.at("supports").get<TermId>(),
      relations.at("attached_to").get<TermId>(), relations.at("in_zone").get<TermId>()};
  const auto &measurements = catalog_state.mechanical.values.at("measurements");
  const catalog::MeasurementBindings measurement_roles{measurements.at("volume").get<TermId>(),
                                                       measurements.at("capacity").get<TermId>()};

  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  auto &objects = objects_state.objects;
  auto &parts = objects_state.parts;

  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;

  auto &ledger = perm_state.ledger;

  const auto authority_read = authority(std::as_const(objects));
  const auto attribution_read = attribution(objects);
  NativeCompletion out;
  switch (operation) {
  case react::ConstructionAction::Create: {
    out = create_instance(objects_state, catalog_state, geometry, values, space_state, perm_state,
                          physics_roles, physics_defaults, call);
    break;
  }
  case react::ConstructionAction::Destroy: {
    out = remove_instance(parts, values, authority_read, attribution_read, ledger, models, scripts,
                          call);
    break;
  }
  case react::ConstructionAction::Reconfigure: {
    out = replace_instance(objects_state, catalog_state, geometry, values, space_state, perm_state,
                           physics_defaults, call);
    break;
  }
  case react::ConstructionAction::ReviseModel: {
    if (const auto error = validate_model_edit(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = revise_model(objects_state, catalog_state, react_state, measurement_roles, geometry,
                       space_state, perm_state, physics_defaults, call);
    break;
  }
  case react::ConstructionAction::PlaceExisting: {
    if (const auto error = validate_place_existing(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = place_existing(objects_state, space_state, relation_terms, perm_state, geometry,
                         catalog_state, physics_roles, physics_defaults, react_state, call);
    break;
  }
  }
  return out;
}
NativeCompletion native_exchange(amber::ObjectState &objects_state, amber::perm::State &perm_state,
                                 const space::Space &space, const DeliveryTargets &delivery,
                                 std::size_t mailbox_max, react::ExchangeAction operation,
                                 const NativeCall &call) {
  auto &parts = objects_state.parts;

  auto &players = perm_state.players;
  auto &groups = perm_state.groups;
  auto &grants = perm_state.grants;
  auto &ledger = perm_state.ledger;

  auto &offers = perm_state.offers;
  auto &trades = perm_state.trades;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto authority_read = amber::authority(std::as_const(objects_state.objects));

  auto permission_bits = amber::permission_flags(objects_state.objects);

  NativeCompletion out;
  switch (operation) {
  case react::ExchangeAction::HoldGoods: {
    if (const auto error = validate_hold_goods(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = hold_goods(perm_state, objects_state, delivery, space, mailbox_max, call);
    break;
  }
  case react::ExchangeAction::Pay: {
    if (const auto error = validate_pay(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = step::pay(ledger, grants, players, identity, authority_read, groups, call);
    break;
  }
  case react::ExchangeAction::Balance: {
    if (const auto error = validate_balance(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = step::balance(ledger, players, identity, authority_read, groups, call);
    break;
  }
  case react::ExchangeAction::Inventory: {
    if (const auto error = validate_inventory(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = inventory(players, call);
    break;
  }
  case react::ExchangeAction::Give: {
    if (const auto error = validate_give(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = give(perm_state, objects_state, space, delivery, mailbox_max, call);
    break;
  }
  case react::ExchangeAction::Offer: {
    if (const auto error = validate_offer(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = offer(perm_state, objects_state, space, call);
    break;
  }
  case react::ExchangeAction::Offers: {
    if (const auto error = validate_offers(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = step::offers(offers, players, identity, authority_read, call);
    break;
  }
  case react::ExchangeAction::Accept: {
    if (const auto error = validate_accept(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = step::accept(perm_state, objects_state, space, delivery, mailbox_max, call);
    break;
  }
  case react::ExchangeAction::CancelOffer: {
    if (const auto error = validate_cancel_offer(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = cancel_offer(perm_state, objects_state, call);
    break;
  }
  case react::ExchangeAction::TradeOpen: {
    if (const auto error = validate_trade_open(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = trade_open(trades, players, identity, delivery, call);
    break;
  }
  case react::ExchangeAction::TradeChange: {
    if (const auto error = validate_trade_change(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = trade_change(perm_state, objects_state, space, call);
    break;
  }
  case react::ExchangeAction::TradeConfirm: {
    if (const auto error = validate_trade_confirm(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = trade_confirm(perm_state, objects_state, space, delivery, call);
    break;
  }
  case react::ExchangeAction::TradeCancel: {
    if (const auto error = validate_trade_cancel(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = trade_cancel(trades, offers, permission_bits, identity, parts, call);
    break;
  }
  }
  return out;
}
NativeCompletion native_permissions(const amber::catalog::State &catalog_state,
                                    const amber::react::State &react_state,
                                    amber::perm::State &perm_state,
                                    amber::ObjectState &objects_state, bool architect,
                                    react::PermissionAction operation, const NativeCall &call) {

  auto &players = perm_state.players;
  auto &groups = perm_state.groups;
  auto &plots = perm_state.plots;
  auto &grants = perm_state.grants;

  auto &object_presets = objects_state.objects.preset;

  NativeCompletion out;
  switch (operation) {
  case react::PermissionAction::Grant: {
    if (const auto error = validate_grant(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = grant(perm_state, objects_state, catalog_state, react_state, call);
    break;
  }
  case react::PermissionAction::Revoke: {
    if (const auto error = validate_revoke(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = revoke(perm_state, objects_state, architect, call);
    break;
  }
  case react::PermissionAction::ListGrants: {
    if (const auto error = validate_list_grants(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = list_grants(grants, players, call);
    break;
  }
  case react::PermissionAction::Preset: {
    if (const auto error = validate_preset(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = preset(objects_state, object_presets, perm_state, call);
    break;
  }
  case react::PermissionAction::GroupCreate: {
    if (const auto error = validate_group_create(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = group_create(groups, players, call);
    break;
  }
  case react::PermissionAction::GroupMember: {
    if (const auto error = validate_group_member(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = group_member(groups, players, call);
    break;
  }
  case react::PermissionAction::PlotPermissions: {
    if (const auto error = validate_plot_permissions(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = plot_permissions(plots, players, groups, call);
    break;
  }
  }
  return out;
}
NativeCompletion native_scripts(amber::catalog::State &catalog_state,
                                amber::react::State &react_state, amber::perm::State &perm_state,
                                const amber::ObjectState &objects_state, TermId start_event,
                                std::span<const std::string> roles, react::ScriptAction operation,
                                const NativeCall &call) {
  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  NativeCompletion out;
  switch (operation) {
  case react::ScriptAction::Install: {
    if (const auto error = editing::validate_install(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::install_script(call, react_state, catalog_state, objects_state, perm_state,
                                  start_event);
    break;
  }
  case react::ScriptAction::Uninstall: {
    if (const auto error = editing::validate_uninstall(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::uninstall_script(call, react_state, values, catalog_state, perm_state,
                                    start_event);
    break;
  }
  case react::ScriptAction::ModelScript: {
    if (const auto error = editing::validate_model_script(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::model_script(call, react_state, values, catalog_state, perm_state, start_event);
    break;
  }
  }
  return out;
}
NativeCompletion native_values(const perm::Players &players, const catalog::Models &models,
                               const react::ValueRead &values, amber::react::State &react_state,
                               std::array<TermId, 4> relation_terms,
                               std::span<const std::string> roles, react::ValueAction operation,
                               const NativeCall &call) {
  auto &properties = react_state.properties;
  auto &processes = react_state.processes;
  auto &instance_settings = react_state.instance_settings;
  auto &model_settings = react_state.model_settings;
  auto &authored_relations = react_state.authored_relations;
  auto &relations = react_state.relations;

  NativeCompletion out;
  switch (operation) {
  case react::ValueAction::SetSetting: {
    if (const auto error = editing::validate_set_setting(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::set_setting(call, instance_settings, values);
    break;
  }
  case react::ValueAction::ModelSetting: {
    if (const auto error = editing::validate_model_setting(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::model_setting(call, model_settings, values, models, players);
    break;
  }
  case react::ValueAction::PatchState: {
    if (const auto error = editing::validate_patch_state(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::patch_state(call, properties, processes, values, roles);
    break;
  }
  case react::ValueAction::Relate: {
    if (const auto error = editing::validate_relate(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::relate(call, authored_relations, relations, values, relation_terms);
    break;
  }
  }
  return out;
}
NativeCompletion native_writing(const perm::Players &players, const catalog::Models &models,
                                catalog::ModelTexts &model_text,
                                catalog::InstanceTexts &instance_text,
                                catalog::AuthoredTexts &authored_text, IdentityRead identity,
                                react::WritingAction operation, const NativeCall &call) {
  NativeCompletion out;
  switch (operation) {
  case react::WritingAction::EditText: {
    if (const auto error = editing::validate_edit_text(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::edit_text(call, instance_text, model_text, identity);
    break;
  }
  case react::WritingAction::ModelText: {
    if (const auto error = editing::validate_model_text(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::model_text(call, model_text, models, identity, players);
    break;
  }
  case react::WritingAction::EditDetail: {
    if (const auto error = editing::validate_edit_detail(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::edit_detail(call, authored_text);
    break;
  }
  }
  return out;
}
NativeCompletion native_registry(amber::catalog::State &catalog_state,
                                 amber::react::State &react_state,
                                 const std::filesystem::path &registry_directory,
                                 const react::ValueRead &values, const GeometryAssets &geometry,
                                 amber::space::State &space_state, PhysicsDefaults physics_defaults,
                                 std::span<const std::string> roles,
                                 react::RegistryAction operation, const NativeCall &call) {
  auto &space = space_state.space;
  const auto &physics = catalog_state.mechanical.values.at("physics");
  const PhysicsRoles physics_roles{
      physics.at("density").get<TermId>(), physics.at("transmission").get<TermId>(),
      physics.value("friction", TermId{}), physics.value("restitution", TermId{})};

  auto &terms = react_state.terms;
  auto &materials = react_state.materials;
  auto &mechanical = catalog_state.mechanical;
  auto &registry = catalog_state.registry;
  auto &rules = react_state.rules;

  NativeCompletion out;
  switch (operation) {
  case react::RegistryAction::DefineTerm: {
    if (const auto error = editing::validate_define_term(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::define_term(call, roles, terms, materials, rules, mechanical, registry,
                               registry_directory);
    break;
  }
  case react::RegistryAction::DefineMaterial: {
    if (const auto error = editing::validate_define_material(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::define_material(call, roles, react_state, values, catalog_state, geometry, space,
                                   physics_roles, physics_defaults, registry_directory);
    break;
  }
  }
  return out;
}
NativeCompletion native_elaboration(amber::catalog::State &catalog_state,
                                    const react::ValueRead &values,
                                    amber::react::State &react_state, amber::net::State &net_state,
                                    WorkQueues &queues, amber::elaborate::State &elaborate_state,
                                    elaborate::FixtureWorker &fixture_worker,
                                    elaborate::FixtureLimits fixture_limits,
                                    react::ElaborationAction operation, const NativeCall &call) {
  std::span<const std::string> roles;
  if (const auto *player = std::get_if<PlayerId>(&call.request.caller);
      player && *player < net_state.profiles.rows.size())
    roles = net_state.profiles.rows[*player].roles;
  const bool architect = std::ranges::find(roles, "architect") != roles.end();
  const bool elaborator = std::ranges::find(roles, "elaborator") != roles.end();

  auto &scripts = catalog_state.scripts;
  auto &authored_text = catalog_state.authored_text;

  auto &profiles = net_state.profiles;
  auto &sessions = net_state.sessions;
  auto &requests = elaborate_state.requests;

  auto &gap_counts = elaborate_state.gap_counts;
  auto &last_gaps = elaborate_state.last_gaps;

  NativeCompletion out;
  switch (operation) {
  case react::ElaborationAction::RequestModel: {
    if (const auto error = validate_request_model(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = request_model(requests, last_gaps, call);
    break;
  }
  case react::ElaborationAction::Requests: {
    if (const auto error = validate_requests(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = step::requests(requests, gap_counts, elaborator, call.hz, call);
    break;
  }
  case react::ElaborationAction::Claim: {
    if (const auto error = validate_claim(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = claim(requests, elaborator, call);
    break;
  }
  case react::ElaborationAction::RuleActivation: {
    if (const auto error = validate_rule_activation(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = rule_activation(scripts, react_state, queues, values, architect || elaborator, call);
    break;
  }
  case react::ElaborationAction::Answer:
  case react::ElaborationAction::Refuse: {
    const auto error = operation == react::ElaborationAction::Refuse
                           ? validate_refuse(call.arguments)
                           : validate_answer(call.arguments);
    if (!error.empty())
      throw std::invalid_argument(error);
    const auto close = [&](AnswerCompletion completion) {
      if (completion.notification) {
        const auto &notice = *completion.notification;
        sessions.event_for_player(notice.requester,
                                  {{"kind", notice.kind},
                                   {"request", "request:" + std::to_string(notice.request)},
                                   {"text", notice.text}});
        const auto target = requests.rows.at(notice.request).target;
        completion.action.changed_objects.push_back(target);
      }
      return std::move(completion.action);
    };
    if (operation == react::ElaborationAction::Refuse)
      out = close(refuse(requests, authored_text, elaborator, call));
    else if (call.arguments.at("kind") == "patch")
      out = close(answer_patch(requests, elaborator, call));
    else if (call.arguments.at("kind") == "detail")
      out = close(answer_detail(requests, authored_text, elaborator, call));
    else {
      const auto id = identifier(call.arguments.at("request"), "request:");
      const auto requester = requests.rows.at(id).requester;
      const auto body =
          requester && requester < profiles.rows.size() ? profiles.rows[requester].body : Handle{};
      out = answer_rule(elaborate_state, scripts, fixture_worker, runtime_version, fixture_limits,
                        elaborator, body, call);
    }
    break;
  }
  }
  return out;
}
} // namespace amber::step
