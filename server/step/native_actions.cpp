#include "action_private.hpp"
#include <stdexcept>

namespace amber::step {
NativeCompletion native_mechanics(
    const Relation &declared_parts, const react::Terms &terms, react::Properties &properties,
    react::Processes &processes, const perm::Players &players, const perm::Plots &plots,
    const perm::Grants &grants, const perm::OwnerLimits &owner_limits, space::Space &space,
    space::PhysicalRelations &physical_relations, MechanicalCauses &mechanical_causes,
    IdentityRead identity, TransformWrite poses, ParentWrite parents, AuthorityRead authority_read,
    SpaceFlags physical_flags, PlotWrite membership, PhysicalRelationTerms physical_terms,
    float reach_m, react::MechanicAction operation, const NativeCall &call) {
  NativeCompletion out;
  switch (operation) {
  case react::MechanicAction::Grasp: {
    out = grasp_action(space, identity, poses, parents, physical_flags, physical_relations,
                       declared_parts, physical_terms, reach_m, mechanical_causes, membership,
                       plots, call);
    break;
  }
  case react::MechanicAction::Release: {
    out = release_action(space, identity, poses, parents, physical_flags, physical_relations,
                         declared_parts, physical_terms, authority_read, owner_limits, players,
                         grants, mechanical_causes, membership, plots, call);
    break;
  }
  case react::MechanicAction::Attach: {
    out = attach_action(space, identity, poses, parents, physical_flags, physical_relations,
                        declared_parts, physical_terms, authority_read, owner_limits, players,
                        grants, reach_m, mechanical_causes, membership, plots, call);
    break;
  }
  case react::MechanicAction::Impulse: {
    out = impulse_action(space, identity, mechanical_causes, call);
    break;
  }
  case react::MechanicAction::Transfer: {
    out = transfer_action(properties, processes, terms, identity, authority_read, owner_limits,
                          players, grants, call);
    break;
  }
  }
  return out;
}
NativeCompletion native_construction(
    Objects &objects, lifecycle::Parts &parts, Relation &declared_parts,
    const catalog::Assets &assets, catalog::Models &models, const catalog::Scripts &scripts,
    catalog::ParameterBlocks &parameters, catalog::ModelTexts &model_text,
    const react::ValueRead &values, react::ModelSettings &model_settings,
    react::AllocationRows &allocations, react::MeterRows &meters,
    react::AssetMeasurements &baked_measurements, perm::Players &players, const perm::Plots &plots,
    const perm::Grants &grants, const perm::Ledger &ledger, const perm::OwnerLimits &owner_limits,
    perm::Offers &offers, perm::Trades &trades, const GeometryAssets &geometry, space::Space &space,
    space::PhysicalRelations &physical_relations,
    std::vector<space::RelationChange> &relation_changes, std::array<TermId, 4> relation_terms,
    PhysicsRoles physics_roles, PhysicsDefaults physics_defaults,
    catalog::MeasurementBindings measurement_roles, react::ConstructionAction operation,
    const NativeCall &call) {
  const auto identity = values.identity;
  const auto poses = transforms(objects);
  const auto parents = amber::parents(objects);
  const auto authority_read = authority(std::as_const(objects));
  const auto attribution_read = attribution(objects);
  const auto physical_flags = space_flags(objects);
  const auto permission_bits = permission_flags(objects);
  const auto membership = plot_membership(objects);
  NativeCompletion out;
  switch (operation) {
  case react::ConstructionAction::Create: {
    out = create_instance(objects, parts, declared_parts, models, parameters, assets, geometry,
                          values, space, poses, parents, physical_flags, physical_relations,
                          relation_changes, authority_read, players, grants, plots, membership,
                          physics_roles, physics_defaults, call);
    break;
  }
  case react::ConstructionAction::Destroy: {
    out = remove_instance(parts, values, authority_read, attribution_read, ledger, models, scripts,
                          call);
    break;
  }
  case react::ConstructionAction::Reconfigure: {
    out = replace_instance(objects, parts, declared_parts, models, parameters, assets, geometry,
                           values, space, poses, parents, physical_flags, physical_relations,
                           relation_changes, authority_read, attribution_read, owner_limits,
                           players, grants, plots, ledger, scripts, membership, physics_roles,
                           physics_defaults, call);
    break;
  }
  case react::ConstructionAction::ReviseModel: {
    if (const auto error = validate_model_edit(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out =
        revise_model(objects, parts, declared_parts, models, parameters, model_text, model_settings,
                     assets, baked_measurements, measurement_roles, geometry, values, space, poses,
                     parents, physical_flags, permission_bits, physical_relations, relation_changes,
                     authority_read, attribution_read, ledger, offers, trades, plots, scripts,
                     membership, physics_roles, physics_defaults, call);
    break;
  }
  case react::ConstructionAction::PlaceExisting: {
    if (const auto error = validate_place_existing(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = place_existing(space, identity, poses, parents, physical_flags, physical_relations, parts,
                         declared_parts, relation_terms, membership, plots, players, grants,
                         authority_read, owner_limits, geometry, models, parameters, assets, values,
                         physics_roles, physics_defaults, allocations, meters, call);
    break;
  }
  }
  return out;
}
NativeCompletion native_exchange(const lifecycle::Parts &parts, const Relation &declared_parts,
                                 perm::Players &players, const perm::Groups &groups,
                                 perm::Grants &grants, perm::Ledger &ledger,
                                 perm::OwnerLimits &owner_limits, perm::Offers &offers,
                                 perm::Trades &trades, const space::Space &space,
                                 const DeliveryTargets &delivery, IdentityRead identity,
                                 ParentRead parents, AuthorityRead authority_read,
                                 AuthorityWrite authority_write, SpaceFlags physical_flags,
                                 PermissionFlags permission_bits, std::size_t mailbox_max,
                                 react::ExchangeAction operation, const NativeCall &call) {
  NativeCompletion out;
  switch (operation) {
  case react::ExchangeAction::Issue: {
    if (const auto error = validate_issue(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = issue(ledger, grants, players, identity, authority_read, groups, call);
    break;
  }
  case react::ExchangeAction::HoldGoods: {
    if (const auto error = validate_hold_goods(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = hold_goods(players, grants, owner_limits, identity, authority_read, parents,
                     physical_flags, declared_parts, delivery, space, mailbox_max, call);
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
    out = give(players, grants, authority_write, permission_bits, owner_limits, space, identity,
               parents, parts, declared_parts, delivery, mailbox_max, call);
    break;
  }
  case react::ExchangeAction::Offer: {
    if (const auto error = validate_offer(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = offer(offers, permission_bits, players, grants, authority_read, owner_limits, space,
                identity, parents, parts, declared_parts, call);
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
    out = step::accept(offers, ledger, grants, players, authority_write, permission_bits,
                       owner_limits, space, identity, parents, parts, declared_parts, delivery,
                       mailbox_max, groups, call);
    break;
  }
  case react::ExchangeAction::CancelOffer: {
    if (const auto error = validate_cancel_offer(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = cancel_offer(offers, permission_bits, players, grants, authority_read, owner_limits,
                       identity, parts, declared_parts, call);
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
    out = trade_change(trades, offers, permission_bits, players, grants, authority_read,
                       owner_limits, space, identity, parents, parts, declared_parts, call);
    break;
  }
  case react::ExchangeAction::TradeConfirm: {
    if (const auto error = validate_trade_confirm(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = trade_confirm(trades, offers, ledger, grants, players, authority_write, permission_bits,
                        owner_limits, space, identity, parents, parts, declared_parts, delivery,
                        call);
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
NativeCompletion
native_permissions(const catalog::Models &models, const catalog::Scripts &scripts,
                   const react::ValueRead &values, const react::RuleVersions &rules,
                   const react::InstanceScripts &installations, perm::Players &players,
                   perm::Groups &groups, perm::Plots &plots, perm::Grants &grants,
                   const perm::OwnerLimits &owner_limits, IdentityRead identity,
                   AuthorityRead authority_read, PermissionFlags permission_bits, bool architect,
                   std::vector<GroupId> &object_groups, std::vector<std::uint32_t> &object_presets,
                   react::PermissionAction operation, const NativeCall &call) {
  NativeCompletion out;
  switch (operation) {
  case react::PermissionAction::Grant: {
    if (const auto error = validate_grant(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = grant(grants, players, identity, authority_read, owner_limits, plots, groups, models,
                scripts, installations, rules, values, architect, call);
    break;
  }
  case react::PermissionAction::Revoke: {
    if (const auto error = validate_revoke(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = revoke(grants, players, identity, authority_read, owner_limits, plots, groups, architect,
                 call);
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
    out = preset(object_groups, object_presets, permission_bits, identity, authority_read,
                 owner_limits, players, groups, call);
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
NativeCompletion native_scripts(catalog::Models &models, const catalog::Scripts &scripts,
                                const react::ValueRead &values, react::SharedColumns &shared,
                                react::GivesTo &gives_to, react::ScriptStates &states,
                                react::Triggers &triggers, react::Subscriptions &subscriptions,
                                react::InstanceScripts &installations,
                                react::AllocationRows &allocations, perm::Players &players,
                                const perm::Groups &groups, const perm::Plots &plots,
                                perm::Grants &grants, const perm::OwnerLimits &owner_limits,
                                AuthorityRead authority_read, TermId start_event,
                                std::span<const std::string> roles, react::ScriptAction operation,
                                const NativeCall &call) {
  NativeCompletion out;
  switch (operation) {
  case react::ScriptAction::Install: {
    if (const auto error = editing::validate_install(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::install_script(call, installations, triggers, subscriptions, states, shared,
                                  gives_to, allocations, values, models, scripts, authority_read,
                                  owner_limits, players, grants, plots, groups, roles, start_event);
    break;
  }
  case react::ScriptAction::Uninstall: {
    if (const auto error = editing::validate_uninstall(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::uninstall_script(call, installations, triggers, subscriptions, states, shared,
                                    gives_to, allocations, values, models, scripts, players, grants,
                                    start_event);
    break;
  }
  case react::ScriptAction::ModelScript: {
    if (const auto error = editing::validate_model_script(call.arguments); !error.empty())
      throw std::invalid_argument(error);
    out = editing::model_script(call, installations, triggers, subscriptions, states, shared,
                                gives_to, allocations, values, models, scripts, players, grants,
                                start_event);
    break;
  }
  }
  return out;
}
NativeCompletion
native_values(const catalog::Models &models, const react::ValueRead &values,
              react::Properties &properties, react::Processes &processes,
              react::InstanceSettings &instance_settings, react::ModelSettings &model_settings,
              react::AuthoredRelations &authored_relations, react::RelationsRead &relations,
              std::array<TermId, 4> relation_terms, std::span<const std::string> roles,
              react::ValueAction operation, const NativeCall &call) {
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
    out = editing::model_setting(call, model_settings, values, models);
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
NativeCompletion native_writing(const catalog::Models &models, catalog::ModelTexts &model_text,
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
    out = editing::model_text(call, model_text, models, identity);
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
NativeCompletion native_registry(
    const catalog::Assets &assets, const catalog::Models &models,
    const catalog::ParameterBlocks &parameters, react::Terms &terms, react::Materials &materials,
    const catalog::MechanicalBindings &mechanical, catalog::RegistryDocument &registry,
    const std::filesystem::path &registry_directory, const react::ValueRead &values,
    const react::RuleVersions &rules, const GeometryAssets &geometry, space::Space &space,
    PhysicsRoles physics_roles, PhysicsDefaults physics_defaults,
    std::span<const std::string> roles, react::RegistryAction operation, const NativeCall &call) {
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
    out = editing::define_material(call, roles, materials, values, models, parameters, assets,
                                   geometry, space, physics_roles, physics_defaults, rules,
                                   mechanical, registry, registry_directory);
    break;
  }
  }
  return out;
}
NativeCompletion
native_elaboration(catalog::Scripts &scripts, catalog::AuthoredTexts &authored_text,
                   const react::ValueRead &values, react::Vm &vm, react::Triggers &triggers,
                   react::RuleVersions &rules, react::Subscriptions &subscriptions,
                   react::AllocationRows &allocations, const net::Profiles &profiles,
                   net::Sessions &sessions, WorkQueues &queues, elaborate::Requests &requests,
                   elaborate::PendingFixtures &fixtures, elaborate::CaptureRefs &captures,
                   elaborate::GapCounts &gap_counts, elaborate::LastGaps &last_gaps,
                   elaborate::FixtureWorker &fixture_worker, bool architect, bool elaborator,
                   elaborate::FixtureLimits fixture_limits, react::ElaborationAction operation,
                   const NativeCall &call) {
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
    out = rule_activation(scripts, rules, triggers, subscriptions, queues, vm, values, allocations,
                          architect || elaborator, call);
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
      out = answer_rule(requests, fixtures, captures, scripts, fixture_worker, runtime_version,
                        fixture_limits, elaborator, body, call);
    }
    break;
  }
  }
  return out;
}
} // namespace amber::step
