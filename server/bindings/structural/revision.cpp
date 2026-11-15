#include "revision.hpp"
#include "bindings/metadata/metadata.hpp"
#include "bindings/scene.hpp"
#include "catalog/revision.hpp"
#include "private.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace amber::bindings {
namespace {
std::vector<Handle> revision_roots(const ModelRevisionRead &read, ModelId model) {
  std::vector<Handle> result;
  for (std::uint32_t slot = 0; slot < read.identity.model.size(); ++slot) {
    auto object = read.identity.at(slot);
    if (object && read.identity.model[slot] == model)
      result.push_back(object);
  }
  return result;
}
catalog::ModelRevisionPlan revision_plan(const ModelRevisionRead &read, ModelId model,
                                         ModelId proposal, std::span<const Handle> roots) {
  std::vector<std::uint32_t> params;
  for (auto object : roots)
    params.push_back(read.params[object.slot()]);
  return catalog::plan_revision(read.models, read.parameters, read.assets, model, proposal, params);
}
void add_mass(std::map<TermId, double> &totals, const ModelRevisionRead &read,
              const catalog::Models &models, const catalog::ParameterBlocks &parameters,
              lifecycle::Binding binding) {
  const auto effective = catalog::resolve_asset(models, parameters, binding.model, binding.params);
  const auto volume = read.assets.rows.at(effective.hash).volume;
  const auto &term = read.terms.rows.at(read.density);
  if (term.kind != react::Kind::Quantity || term.source != react::Source::Intrinsic)
    throw std::invalid_argument("model revision density must be an intrinsic quantity");
  const auto density = react::read_intrinsic(read.materials, binding.material, term);
  if (!density.present || !density.value.is_numeric() || density.value.numeric() <= 0 ||
      !std::isfinite(density.value.numeric()) || volume < 0 || !std::isfinite(volume))
    throw std::invalid_argument("model revision requires finite nonnegative disjoint mass");
  const auto mass = volume * density.value.numeric();
  auto &sum = totals[binding.material];
  if (!std::isfinite(mass) || !std::isfinite(sum + mass))
    throw std::invalid_argument("model revision mass overflow");
  sum += mass;
}
lifecycle::Replacement binding_plan(const ModelRevisionRead &read, const catalog::Models &models,
                                    const catalog::ParameterBlocks &parameters,
                                    const catalog::ModelRevisionPlan &revision, Handle root) {
  const auto next = catalog::revision_variant(revision, read.params[root.slot()]).after_params;
  auto assembly = lifecycle::expand(models, parameters, read.assets, revision.model, next);
  return lifecycle::plan_bindings(read.models, read.parts, read.identity, read.params, root,
                                  assembly, read.identity.material[root.slot()]);
}
} // namespace
RevisionMasses measure_model_change(const ModelRevisionRead &read, ModelId model,
                                    ModelId proposal) {
  const auto roots = revision_roots(read, model);
  const auto revision = revision_plan(read, model, proposal, roots);
  auto models = read.models;
  auto parameters = read.parameters;
  catalog::ModelTexts texts;
  react::ModelSettings settings;
  catalog::commit_revision(models, parameters, texts, settings, revision);
  RevisionMasses result;
  result.count = roots.size();
  for (auto root : roots) {
    const auto plan = binding_plan(read, models, parameters, revision, root);
    const auto before = lifecycle::collect(read.parts, read.identity, root);
    const auto removed = lifecycle::collect(read.parts, read.identity, plan.removed);
    std::unordered_set<std::uint64_t> absent;
    for (auto h : removed)
      absent.insert(h.bits);
    for (auto h : before) {
      const lifecycle::Binding current{read.identity.model[h.slot()],
                                       read.identity.material[h.slot()], read.params[h.slot()]};
      add_mass(result.before, read, read.models, read.parameters, current);
      if (absent.contains(h.bits))
        continue;
      const auto next = std::ranges::find(plan.retained, h, &lifecycle::Rebinding::object);
      add_mass(result.after, read, models, parameters,
               next == plan.retained.end() ? current : next->after);
    }
    for (const auto &part : plan.added)
      add_mass(result.after, read, models, parameters, {part.model, part.material, 0});
  }
  return result;
}

drive::StructuralCompletion revise_model(void *context, const drive::StructuralCall &call) {
  using namespace structural_detail;
  auto &operation = *static_cast<ModelRevisionOperation *>(context);
  auto &replacement = operation.replacement;
  const auto &read = operation.read;
  drive::StructuralCompletion out;
  if (!call.handler.world_rule || !read.identity.valid(call.invocation.receiver) ||
      !call.request.revision || !call.invocation.action_caller ||
      !std::holds_alternative<PlayerId>(*call.invocation.action_caller)) {
    out.error = "definition editing requires an immediate model author and approved world rule";
    return out;
  }
  const auto request = *call.request.revision;
  if (!request.model || request.model >= read.models.rows.size() ||
      read.models.rows[request.model].id != request.model) {
    out.error = "unknown model in definition edit";
    return out;
  }
  const auto author = std::get<PlayerId>(*call.invocation.action_caller);
  const auto &original = read.models.rows[request.model];
  if (original.author != author) {
    out.error = "only the model author can revise its definition";
    return out;
  }
  if (original.source_hash != request.expected_source_hash) {
    out.error = "model source changed; rebake the proposed definition";
    return out;
  }
  const auto roots = revision_roots(read, request.model);
  const auto revision = revision_plan(read, request.model, request.proposal, roots);
  if (!revision.changed)
    return out;
  out.error = check_resources(replacement.resources, call);
  if (!out.error.empty())
    return out;
  auto models = operation.models;
  auto parameters = operation.parameters;
  auto texts = operation.texts;
  auto settings = operation.settings;
  auto measurements = operation.measurements;
  catalog::commit_revision(models, parameters, texts, settings, revision);
  catalog::project_revision_measurements(measurements, revision, read.assets,
                                         operation.measurement_roles);
  const auto &values = replacement.geometry.values;
  const react::ValueRead prospective{
      values.terms,      values.identity,      values.plots,
      values.params,     values.materials,     measurements,
      values.properties, values.processes,     values.instance_settings,
      settings,          values.plot_settings, values.world_settings,
      values.shared,     values.relations,     values.runtime,
      values.density};
  const ConstructionCatalog catalog{models, parameters, read.assets};
  std::vector<PreparedReplacement> prepared;
  std::vector<Handle> retiring, removed;
  std::unordered_set<std::uint64_t> native_roots;
  for (auto root : roots) {
    const auto after = catalog::revision_variant(revision, read.params[root.slot()]).after_params;
    auto assembly = lifecycle::expand(models, parameters, read.assets, request.model, after);
    auto plan =
        lifecycle::plan_replace(read.models, read.parts, read.identity, replacement.transforms,
                                read.params, root, assembly, read.identity.material[root.slot()]);
    auto ready = prepare_replacement(replacement, call, std::move(plan), catalog, prospective);
    if (!native_roots.insert(ready.mechanical_root.bits).second)
      throw std::invalid_argument("definition edit has overlapping physical assemblies");
    retiring.insert(retiring.end(), ready.before.begin(), ready.before.end());
    removed.insert(removed.end(), ready.removed.begin(), ready.removed.end());
    prepared.push_back(std::move(ready));
  }
  std::vector<space::VisibleCustody> forecast;
  const auto &prerequisite = operation.prerequisite;
  if (bool(prerequisite.preflight) != bool(prerequisite.commit))
    throw std::logic_error("definition prerequisite requires a preflight/commit pair");
  if (prerequisite.preflight &&
      !prerequisite.preflight(prerequisite.local, retiring, removed, forecast, out.error))
    return out;
  for (const auto &change : forecast)
    if (change.parent && std::ranges::find(removed, change.parent) != removed.end()) {
      out.error = "definition would retire a restored custody parent";
      return out;
    }
  std::size_t required = 0, characters = 0;
  for (auto &ready : prepared) {
    std::optional<space::Motion> root_motion;
    const auto future =
        std::ranges::find(forecast, ready.mechanical_root, &space::VisibleCustody::goods);
    if (future != forecast.end() && future->parent) {
      const auto current =
          space::body_motion(replacement.geometry.space, read.identity, ready.mechanical_root);
      if (!current || current->motion != space::Motion::Character)
        root_motion = space::Motion::Kinematic;
      ready.promote |= future->grasp && std::ranges::any_of(ready.bodies, [&](const auto &body) {
                         return body.object != ready.mechanical_root;
                       });
    }
    ready.capacity =
        space::can_rebuild_assembly(replacement.geometry.space, ready.bodies, ready.sensors,
                                    ready.before, ready.promote, root_motion);
    if (ready.capacity.status != space::Status::Ok &&
        ready.capacity.status != space::Status::Capacity) {
      out.error = "definition physics: " + std::string(space::status_text(ready.capacity.status));
      return out;
    }
    if (ready.in_scene) {
      required += ready.capacity.required_native;
      for (const auto &body : ready.bodies)
        characters += body.motion == space::Motion::Character;
    }
  }
  // One credit calculation counts actual aggregates/query bodies/sensors uniquely.
  // Independent per-assembly capacity checks cannot establish the batch invariant.
  const auto availability = space::can_add(replacement.geometry.space, {}, 0, retiring);
  auto character_slots = space::capacity(replacement.geometry.space).characters_available;
  for (auto object : retiring)
    if (const auto mode = space::body_motion(replacement.geometry.space, read.identity, object);
        mode && mode->motion == space::Motion::Character)
      ++character_slots;
  if (required > availability.available_native || characters > character_slots) {
    out.error = "definition physics: capacity";
    return out;
  }
  if (!can_retire_objects(replacement.lifecycle, removed, out.error))
    return out;
  react::DeferredEffects prerequisite_effects;
  if (prerequisite.commit)
    prerequisite.commit(prerequisite.local, retiring, call.invocation, prerequisite_effects);
  completed_effects(out, std::move(prerequisite_effects));
  std::vector<space::AssemblyEdit> edits;
  for (const auto &ready : prepared)
    edits.push_back(space::extract_assembly(replacement.geometry.space, read.identity,
                                            ready.mechanical_root,
                                            std::span(ready.before).subspan(1)));
  catalog::commit_revision(operation.models, operation.parameters, operation.texts,
                           operation.settings, revision);
  catalog::project_revision_measurements(operation.measurements, revision, read.assets,
                                         operation.measurement_roles);
  std::vector<ModelId> changed_models{request.model};
  for (const auto &alias : revision.aliases)
    changed_models.push_back(alias.model.id);
  operation.publication.definitions_changed(changed_models);
  react::DeferredEffects effects;
  commit_resources(replacement.resources, call, effects);
  completed_effects(out, std::move(effects));
  for (std::size_t i = 0; i < prepared.size(); ++i)
    commit_replacement(replacement, call, prepared[i], edits[i], author, out);
  out.changed = true;
  auto event = structural_record(call, record::Kind::System, "model_definition", roots);
  event.detail = {{"model", "m:" + std::to_string(request.model)},
                  {"proposal", "m:" + std::to_string(request.proposal)},
                  {"before_source", revision.before_source},
                  {"after_source", revision.after.source_hash},
                  {"before_asset", revision.before_asset},
                  {"after_asset", revision.after.asset}};
  const auto &proposal = read.models.rows.at(request.proposal);
  event.credits.push_back({proposal.author, proposal.created_s, false});
  out.records.insert(out.records.begin(), std::move(event));
  return out;
}
} // namespace amber::bindings
