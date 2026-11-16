#include "catalog.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace amber::lifecycle {
namespace {
template <std::size_t N> std::array<float, N> coordinates(const nlohmann::json &v) {
  auto result = v.get<std::array<float, N>>();
  if (!std::all_of(result.begin(), result.end(), [](float x) { return std::isfinite(x); }))
    throw std::runtime_error("non-finite part transform");
  return result;
}
Binding binding(IdentityRead identity, const std::vector<std::uint32_t> &params, Handle object) {
  return {identity.model[object.slot()], identity.material[object.slot()], params[object.slot()]};
}
} // namespace
AssemblyTemplate expand(const catalog::Models &models, const catalog::ParameterBlocks &params,
                        const catalog::Assets &assets, ModelId model, std::uint32_t block) {
  const auto asset = catalog::resolve_asset(models, params, model, block);
  const auto &manifest = assets.rows.at(asset.hash).manifest;
  AssemblyTemplate result{model, block, models.rows[model].material, {}};
  for (const auto &[name, child] : catalog::part_models(models, params, model, block)) {
    const auto &entry = manifest.at("parts").at(name);
    const auto p = coordinates<3>(entry.at("position"));
    const auto q = coordinates<4>(entry.at("rotation"));
    const double norm = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] +
                                  double(q[3]) * q[3]);
    if (norm == 0)
      throw std::runtime_error("zero part quaternion");
    result.parts.push_back(
        {name,
         child,
         models.rows.at(child).material,
         {p[0], p[1], p[2]},
         {float(q[0] / norm), float(q[1] / norm), float(q[2] / norm), float(q[3] / norm)}});
  }
  return result;
}
Replacement plan_bindings(const catalog::Models &models, const Parts &parts, IdentityRead identity,
                          const std::vector<std::uint32_t> &params, Handle root,
                          const AssemblyTemplate &assembly, std::optional<TermId> material) {
  if (!identity.valid(root))
    throw std::invalid_argument("cannot replace a stale object");
  Replacement result;
  result.root = root;
  result.original = binding(identity, params, root);
  if (const auto *names = parts.get(root.slot()))
    result.original_parts = *names;
  result.retained.push_back(
      {root,
       result.original,
       {assembly.model, material.value_or(result.original.material), assembly.params},
       {},
       {}});
  for (const auto &part : assembly.parts) {
    const auto found = std::find_if(result.original_parts.begin(), result.original_parts.end(),
                                    [&](const auto &p) { return p.name == part.name; });
    if (found == result.original_parts.end()) {
      result.added.push_back(part);
      continue;
    }
    if (!identity.valid(found->object))
      throw std::logic_error("declared part has stale identity");
    const auto old = binding(identity, params, found->object);
    const auto default_material = models.rows.at(old.model).material;
    const auto next_material = old.material == default_material ? part.material : old.material;
    result.retained.push_back({found->object, old, {part.model, next_material, 0}, {}, {}});
  }
  for (const auto &part : result.original_parts)
    if (std::none_of(assembly.parts.begin(), assembly.parts.end(),
                     [&](const auto &p) { return p.name == part.name; }))
      result.removed.push_back(part.object);
  return result;
}
Replacement plan_replace(const catalog::Models &models, const Parts &parts, IdentityRead identity,
                         TransformRead transforms, const std::vector<std::uint32_t> &params,
                         Handle root, const AssemblyTemplate &assembly,
                         std::optional<TermId> material) {
  auto result = plan_bindings(models, parts, identity, params, root, assembly, material);
  result.retained.front().position = transforms.position[root.slot()];
  result.retained.front().rotation = transforms.rotation[root.slot()];
  for (auto &row : std::span(result.retained).subspan(1)) {
    const auto old = std::ranges::find(result.original_parts, row.object, &NamedPart::object);
    const auto part = std::ranges::find(assembly.parts, old->name, &PartTemplate::name);
    row.position =
        transforms.position[root.slot()] + rotate(transforms.rotation[root.slot()], part->position);
    row.rotation = compose(transforms.rotation[root.slot()], part->rotation);
  }
  return result;
}
} // namespace amber::lifecycle
