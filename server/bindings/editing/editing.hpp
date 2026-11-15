#pragma once

#include "control/control.hpp"
#include "drive/drive.hpp"

namespace amber::bindings {
struct MetadataChanges;
struct RegistryPersistence;
struct MaterialRefresh;
} // namespace amber::bindings
namespace amber::bindings::editing {
using Json = nlohmann::json;
struct Declarations {
  TermId install = 0, uninstall = 0, model_script = 0, edit_text = 0, model_text = 0,
         set_setting = 0, model_setting = 0, patch_state = 0, relate = 0, define_term = 0,
         define_material = 0, edit_detail = 0;
};
// Run before registry pinning and initial script preparation.
Declarations declare_editing(react::Terms &, command::NativeActions &);
struct EditAccess {
  IdentityRead identity;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
};
// Owns instance installations and the model-default script projection together.
struct ScriptEditing {
  install::InstanceScripts &instances;
  react::Triggers &triggers;
  react::Subscriptions &subscriptions;
  install::WatchLinks &links;
  react::ScriptStates &states;
  react::SharedColumns &shared;
  react::GivesTo &gives;
  const react::AllocationRows &allocations;
  const react::ValueRead &values;
  catalog::Models &models;
  const catalog::Scripts &scripts;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  perm::Players &players;
  perm::Grants &grants;
  const perm::Plots &plots;
  const control::Profiles &profiles;
  TermId start_event = 0;
  MetadataChanges &changes;
  const perm::Groups &groups;
};
struct TextEditing {
  catalog::InstanceTexts &instance_texts;
  catalog::ModelTexts &model_texts;
  catalog::AuthoredTexts &details;
  const catalog::Models &models;
  EditAccess access;
  MetadataChanges &changes;
};
struct PropertyEditing {
  react::InstanceSettings &instance_settings;
  react::ModelSettings &model_settings;
  react::Properties &properties;
  react::Processes &processes;
  react::AuthoredRelations &relations;
  react::RelationsRead &projection;
  const react::ValueRead &values;
  const catalog::Models &models;
  const control::Profiles &profiles;
  EditAccess access;
  std::array<TermId, 4> mechanical_relations{};
  MetadataChanges &changes;
};
struct RegistryEditing {
  react::Terms &terms;
  react::Materials &materials;
  const react::ValueRead &values;
  const control::Profiles &profiles;
  RegistryPersistence &persistence;
  MetadataChanges &changes;
  MaterialRefresh &physics;
};
// Registers all twelve editing actions; contexts outlive the operation registry.
void register_actions(drive::NativeOperations &, const Declarations &, ScriptEditing &,
                      TextEditing &, PropertyEditing &, RegistryEditing &);
} // namespace amber::bindings::editing
