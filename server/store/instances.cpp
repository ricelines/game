#include "store.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace amber::lifecycle {
namespace {
template <std::size_t N> std::array<float, N> coordinates(const nlohmann::json &v) {
  auto result = v.get<std::array<float, N>>();
  if (!std::all_of(result.begin(), result.end(), [](float x) { return std::isfinite(x); }))
    throw std::runtime_error("non-finite part transform");
  return result;
}
void edge(Relation &relation, Handle from, Handle to) {
  auto add = [](auto &column, std::uint32_t slot, Handle other) {
    if (auto *row = column.get(slot))
      row->push_back(other);
    else
      column.set(slot, std::vector<Handle>{other});
  };
  add(relation.out, from.slot(), to);
  add(relation.in, to.slot(), from);
}
void erase_edges(Relation &relation, Handle object) {
  auto erase = [](auto &column, Handle subject, Handle other) {
    auto *row = column.get(subject.slot());
    if (!row)
      return;
    std::erase(*row, other);
    if (row->empty())
      column.erase(subject.slot());
  };
  if (auto *row = relation.out.get(object.slot())) {
    for (auto other : *row)
      erase(relation.in, other, object);
    relation.out.erase(object.slot());
  }
  if (auto *row = relation.in.get(object.slot())) {
    for (auto other : *row)
      erase(relation.out, other, object);
    relation.in.erase(object.slot());
  }
}
Binding binding(IdentityRead identity, const std::vector<std::uint32_t> &params, Handle object) {
  return {identity.model[object.slot()], identity.material[object.slot()], params[object.slot()]};
}
ObjectInit copy_init(const Objects &objects, Handle object) {
  const auto s = object.slot();
  return {objects.position[s], objects.rotation[s], objects.model[s],        objects.material[s],
          objects.owner[s],    objects.maker[s],    objects.created_step[s], objects.group[s],
          objects.preset[s],   objects.plot[s],     objects.parent[s],       objects.flags[s],
          objects.params[s]};
}
ObjectInit child_init(ObjectInit parent, Handle root, const PartTemplate &part) {
  parent.position = parent.position + rotate(parent.rotation, part.position);
  parent.rotation = compose(parent.rotation, part.rotation);
  parent.model = part.model;
  parent.material = part.material;
  parent.params = 0;
  parent.parent = root;
  parent.flags &= ~(Body | Held | Escrow);
  return parent;
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
Created instantiate(Objects &objects, Parts &parts, Relation &declared,
                    const AssemblyTemplate &assembly, ObjectInit init) {
  init.model = assembly.model;
  init.params = assembly.params;
  if (!init.material)
    init.material = assembly.material;
  Created result;
  result.root = create(objects, init);
  result.objects.push_back(result.root);
  std::vector<NamedPart> children;
  for (const auto &part : assembly.parts) {
    const auto child = create(objects, child_init(init, result.root, part));
    children.push_back({part.name, child});
    result.objects.push_back(child);
    edge(declared, result.root, child);
  }
  if (!children.empty())
    parts.set(result.root.slot(), std::move(children));
  return result;
}
std::vector<Handle> collect(const Parts &parts, IdentityRead identity,
                            std::span<const Handle> roots) {
  std::vector<Handle> result;
  std::unordered_set<std::uint64_t> visited;
  for (auto object : roots)
    if (identity.valid(object) && visited.insert(object.bits).second)
      result.push_back(object);
  for (std::size_t i = 0; i < result.size(); ++i)
    if (const auto *children = parts.get(result[i].slot()))
      for (const auto &child : *children)
        if (identity.valid(child.object) && visited.insert(child.object.bits).second)
          result.push_back(child.object);
  return result;
}
std::vector<Handle> collect(const Parts &parts, IdentityRead identity, Handle root) {
  return collect(parts, identity, std::span{&root, 1});
}
void release_rows(Objects &objects, Parts &parts, Relation &declared,
                  std::span<const Handle> closed) {
  // External owners have finished reading valid identities. Remove both incident
  // directions before any slot becomes available for allocation.
  for (auto object : closed) {
    if (const auto *parents = declared.in.get(object.slot()))
      for (auto parent : *parents)
        if (auto *siblings = parts.get(parent.slot())) {
          std::erase_if(*siblings, [&](const auto &p) { return p.object == object; });
          if (siblings->empty())
            parts.erase(parent.slot());
        }
    erase_edges(declared, object);
    parts.erase(object.slot());
  }
  for (auto it = closed.rbegin(); it != closed.rend(); ++it)
    destroy(objects, *it);
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
Created apply_replacement(Objects &objects, Parts &parts, Relation &declared,
                          const Replacement &plan, PlayerId edit_maker, Step now) {
  for (auto removed : plan.removed) {
    erase_edges(declared, removed);
    parts.erase(removed.slot());
  }
  std::vector<NamedPart> expected;
  for (const auto &row : plan.original_parts)
    if (std::find(plan.removed.begin(), plan.removed.end(), row.object) == plan.removed.end())
      expected.push_back(row);
  for (const auto &row : plan.retained) {
    const auto s = row.object.slot();
    objects.model[s] = row.after.model;
    objects.material[s] = row.after.material;
    objects.params[s] = row.after.params;
    objects.position[s] = row.position;
    objects.rotation[s] = row.rotation;
  }
  const auto init = copy_init(objects, plan.root);
  Created result{plan.root, {}};
  for (const auto &part : plan.added) {
    auto child = child_init(init, plan.root, part);
    child.maker = edit_maker;
    child.created_step = now;
    const auto object = create(objects, child);
    expected.push_back({part.name, object});
    result.objects.push_back(object);
    edge(declared, plan.root, object);
  }
  std::sort(expected.begin(), expected.end(),
            [](const auto &a, const auto &b) { return a.name < b.name; });
  if (expected.empty())
    parts.erase(plan.root.slot());
  else
    parts.set(plan.root.slot(), std::move(expected));
  return result;
}
void rebuild_declared_parts(const Parts &parts, IdentityRead identity, Relation &declared) {
  declared = {};
  for (std::size_t i = 0; i < parts.value.size(); ++i) {
    const auto root = identity.at(parts.slot_of[i]);
    for (const auto &part : parts.value[i]) {
      if (!identity.valid(root) || !identity.valid(part.object))
        throw std::runtime_error("snapshot contains a stale declared part");
      edge(declared, root, part.object);
    }
  }
}
} // namespace amber::lifecycle
