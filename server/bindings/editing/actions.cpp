#include "common.hpp"

namespace amber::bindings::editing {
void register_actions(drive::NativeOperations &operations, const Declarations &declared,
                      ScriptEditing &scripts, TextEditing &texts, PropertyEditing &properties,
                      RegistryEditing &registry) {
  const auto bind = [&](TermId term, auto validate, auto run, auto &context) {
    drive::register_native(operations, term, {validate, run, &context});
  };
  bind(declared.install, validate_install, install_script, scripts);
  bind(declared.uninstall, validate_uninstall, uninstall_script, scripts);
  bind(declared.model_script, validate_model_script, model_script, scripts);
  bind(declared.edit_text, validate_edit_text, edit_text, texts);
  bind(declared.model_text, validate_model_text, model_text, texts);
  bind(declared.edit_detail, validate_edit_detail, edit_detail, texts);
  bind(declared.set_setting, validate_set_setting, set_setting, properties);
  bind(declared.model_setting, validate_model_setting, model_setting, properties);
  bind(declared.patch_state, validate_patch_state, patch_state, properties);
  bind(declared.relate, validate_relate, relate, properties);
  bind(declared.define_term, validate_define_term, define_term, registry);
  bind(declared.define_material, validate_define_material, define_material, registry);
}
} // namespace amber::bindings::editing
