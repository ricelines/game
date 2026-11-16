#include "catalog/catalog.hpp"
#include "react.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace amber::react {
ValueRead value_read(const ObjectState &objects, const catalog::State &catalog,
                     const State &react) {
  // The relation projection is already bound to the authored and physical owner tables.
  const auto measurements = catalog.mechanical.values.find("measurements");
  const auto binding = [&](const char *name) {
    return measurements == catalog.mechanical.values.end() ? TermId{}
                                                           : measurements->value(name, TermId{});
  };
  return {react.terms,
          identity(objects.objects),
          objects.objects.plot,
          objects.objects.params,
          react.materials,
          catalog.baked_measurements,
          react.properties,
          react.processes,
          react.instance_settings,
          react.model_settings,
          react.plot_settings,
          react.world_settings,
          react.shared,
          react.relations,
          react.runtime_measurements,
          binding("density"),
          binding("capacity")};
}

namespace {
Answer lookup(const Values &values, TermId term) {
  auto found = values.find(term);
  return found == values.end() ? Answer{} : Answer{true, found->second};
}
template <class T>
const T *column(const std::vector<SparseColumn<T>> &cols, TermId term, Handle h) {
  return term < cols.size() ? cols[term].get(h.slot()) : nullptr;
}
void dirty(DeferredEffects &effects, Handle subject, TermId term) {
  effects.ordered.push_back(react::WatchWork{{subject, term}, {}});
  effects.changed = true;
}
void state_term(const Term &term) {
  if ((term.kind != Kind::Quantity && term.kind != Kind::Flag) || term.source != Source::State ||
      term.combine != Combine::None)
    throw std::invalid_argument("term is not writable state");
}
void size_columns(Properties &properties, Processes &processes, TermId term) {
  if (properties.size() <= term)
    properties.resize(term + 1);
  if (processes.size() <= term)
    processes.resize(term + 1);
}
double combined(const Shared &shared, Combine combine) {
  if (shared.parts.empty())
    return 0;
  if (combine == Combine::Max)
    return std::max_element(shared.parts.begin(), shared.parts.end(),
                            [](auto &a, auto &b) { return a.amount < b.amount; })
        ->amount;
  if (combine == Combine::Any)
    return std::any_of(shared.parts.begin(), shared.parts.end(),
                       [](auto &part) { return part.amount != 0; })
               ? 1
               : 0;
  return std::accumulate(shared.parts.begin(), shared.parts.end(), 0.0,
                         [](double sum, auto &part) { return sum + part.amount; });
}
} // namespace
Answer read_intrinsic(const Materials &materials, TermId material, const Term &term) {
  auto found = materials.rows.find(material);
  return found == materials.rows.end() ? Answer{} : lookup(found->second, term.id);
}
Answer read_baked(const AssetMeasurements &assets, ModelId model, const Term &term,
                  std::uint32_t params) {
  const auto &rows = params ? assets.parameter_rows : assets.rows;
  auto found = rows.find(params ? params : model);
  if (found == rows.end())
    return {};
  if (term.measurement == MeasurementRole::Volume)
    return {true, Value(found->second.volume)};
  return lookup(found->second.values, term.id);
}
double read_mass(double volume, double density) {
  return volume * density;
}
double process_value(const Process &p, Step now, double hz) {
  return std::clamp(p.v0 + p.rate * (static_cast<double>(now) - static_cast<double>(p.step0)) / hz,
                    p.lo, p.hi);
}
Answer read_state(const Properties &props, const Processes &processes, Handle h, TermId term,
                  Step now, double hz) {
  if (const auto *p = column(processes, term, h))
    return {true, Value(process_value(*p, now, hz))};
  if (const auto *v = column(props, term, h))
    return {true, *v};
  return {};
}
Answer read_setting(const InstanceSettings &instance, const ModelSettings &models,
                    const PlotSettings &plots, const WorldSettings &world, Handle h, ModelId model,
                    PlotId plot, TermId term) {
  if (const auto *v = column(instance.columns, term, h))
    return {true, *v};
  if (auto it = models.rows.find(model); it != models.rows.end())
    if (auto value = lookup(it->second, term); value.present)
      return value;
  if (auto it = plots.rows.find(plot); it != plots.rows.end())
    if (auto value = lookup(it->second, term); value.present)
      return value;
  return lookup(world.values, term);
}
Answer read_shared(const SharedColumns &shared, const RelationsRead &relations, Handle h,
                   const Term &term) {
  if (const auto *v = column(shared, term.id, h))
    return {true, Value(v->total)};
  if (term.along) {
    auto relation = relations.columns.find(term.along);
    if (relation != relations.columns.end())
      if (auto *edges = relation->second->out.get(h.slot()); edges && !edges->empty())
        if (const auto *v = column(shared, term.id, edges->front()))
          return {true, Value(v->total)};
  }
  return {};
}
Answer read_runtime_measurement(const RuntimeMeasurements &measurements, FactKey key) {
  auto it = measurements.values.find(key);
  return it == measurements.values.end() ? Answer{} : Answer{true, it->second};
}
Answer read(const ValueRead &v, Handle object, TermId id, Step now, double hz) {
  if (!v.identity.valid(object) || id == 0 || id >= v.terms.rows.size())
    return {};
  const auto &term = v.terms.rows[id];
  if (term.combine != Combine::None)
    return read_shared(v.shared, v.relations, object, term);
  const auto slot = object.slot();
  switch (term.source) {
  case Source::Intrinsic:
    return read_intrinsic(v.materials, v.identity.material[slot], term);
  case Source::Measured:
    if (term.measurement == MeasurementRole::Runtime)
      return read_runtime_measurement(v.runtime, {object, id});
    if (term.measurement == MeasurementRole::Mass) {
      const auto &rows = v.params[slot] ? v.assets.parameter_rows : v.assets.rows;
      auto asset = rows.find(v.params[slot] ? v.params[slot] : v.identity.model[slot]);
      if (asset == rows.end() || !v.density || v.density >= v.terms.rows.size())
        return {};
      auto density =
          read_intrinsic(v.materials, v.identity.material[slot], v.terms.rows[v.density]);
      return density.present
                 ? Answer{true, Value(read_mass(asset->second.volume, density.value.numeric()))}
                 : Answer{};
    }
    return read_baked(v.assets, v.identity.model[slot], term, v.params[slot]);
  case Source::State:
    return read_state(v.properties, v.processes, object, id, now, hz);
  case Source::Setting:
    return read_setting(v.instance_settings, v.model_settings, v.plot_settings, v.world_settings,
                        object, v.identity.model[slot], v.plots[slot], id);
  }
  return {};
}
bool set_state(Properties &props, Processes &processes, const Term &term, Handle self, Value value,
               Step now, double hz, DeferredEffects &effects) {
  state_term(term);
  if (!value.present())
    throw std::invalid_argument("state value cannot be absent");
  size_columns(props, processes, term.id);
  const auto before = read_state(props, processes, self, term.id, now, hz);
  bool stopped = false;
  if (auto *process = processes[term.id].get(self.slot())) {
    if (!value.is_numeric())
      throw std::invalid_argument("process value must be numeric");
    stopped = process->rate != 0;
    process->v0 = value.numeric();
    process->step0 = now;
    process->rate = 0;
    process->lo = process->v0;
    process->hi = process->v0;
  }
  props[term.id].set(self.slot(), value);
  const bool changed = !before.present || !(before.value == value) || stopped;
  if (changed)
    dirty(effects, self, term.id);
  return changed;
}
bool set_rate(amber::react::State &react_state, const Term &term, Handle self, double rate,
              double lo, double hi, Step now, double hz, DeferredEffects &effects) {

  auto &props = react_state.properties;
  auto &processes = react_state.processes;

  state_term(term);
  if (lo > hi || !std::isfinite(rate) || !std::isfinite(lo) || !std::isfinite(hi))
    throw std::invalid_argument("invalid process bounds/rate");
  size_columns(props, processes, term.id);
  const auto before = read_state(props, processes, self, term.id, now, hz);
  const double current = before.present ? before.value.numeric() : 0;
  const auto *old = processes[term.id].get(self.slot());
  if (old && old->rate == rate && old->lo == lo && old->hi == hi)
    return false;
  const bool dynamics_changed =
      old ? old->rate != rate || (rate != 0 && (old->lo != lo || old->hi != hi)) : rate != 0;
  const double initial = std::clamp(current, lo, hi);
  processes[term.id].set(self.slot(), Process{initial, now, rate, lo, hi});
  // Rate changes also re-arm subscribed threshold timers even if the current answer is equal.
  effects.ordered.push_back(react::WatchWork{{self, term.id}, {}});
  const bool changed = !before.present || current != initial || dynamics_changed;
  effects.changed |= changed;
  return changed;
}
bool transfer_state(amber::react::State &react_state, const Term &term, Handle from, Handle to,
                    double amount, Step now, double hz, DeferredEffects &effects) {
  auto &props = react_state.properties;
  auto &processes = react_state.processes;

  state_term(term);
  if (!term.conserved || term.kind != Kind::Quantity || amount < 0 || !std::isfinite(amount))
    throw std::invalid_argument("invalid conserved transfer");
  if (from == to || amount == 0)
    return false;
  const auto a = read_state(props, processes, from, term.id, now, hz),
             b = read_state(props, processes, to, term.id, now, hz);
  if (!a.present || !a.value.is_numeric() || (b.present && !b.value.is_numeric()) ||
      a.value.numeric() < amount)
    return false;
  const double debit = a.value.numeric() - amount;
  const double credit = (b.present ? b.value.numeric() : 0) + amount;
  if (!std::isfinite(debit) || !std::isfinite(credit))
    throw std::invalid_argument("transfer endpoints must remain finite");
  size_columns(props, processes, term.id);
  auto *source = processes[term.id].get(from.slot());
  auto *destination = processes[term.id].get(to.slot());
  if ((source && (debit < source->lo || debit > source->hi)) ||
      (destination && (credit < destination->lo || credit > destination->hi)))
    throw std::invalid_argument("transfer would exceed existing process bounds");
  auto apply = [&](Handle target, Process *process, double value) {
    if (process) {
      process->v0 = value;
      process->step0 = now;
    }
    props[term.id].set(target.slot(), Value(value));
    dirty(effects, target, term.id);
  };
  apply(from, source, debit);
  apply(to, destination, credit);

  return true;
}
bool set_contribution(SharedColumns &columns, GivesTo &gives, const Term &term, Handle from,
                      Handle subject, double amount, DeferredEffects &effects) {
  if (term.combine == Combine::None || !std::isfinite(amount))
    throw std::invalid_argument("term is not a combined quantity");
  if (columns.size() <= term.id)
    columns.resize(term.id + 1);
  auto &column = columns[term.id];
  auto *row = column.get(subject.slot());
  if (!row) {
    if (amount == 0)
      return false;
    column.set(subject.slot(), Shared{});
    row = column.get(subject.slot());
  }
  auto found =
      std::find_if(row->parts.begin(), row->parts.end(), [&](auto &p) { return p.from == from; });
  if ((found == row->parts.end() && amount == 0) ||
      (found != row->parts.end() && found->amount == amount))
    return false;
  if (found == row->parts.end() && amount != 0) {
    row->parts.push_back({from, amount});
    auto *reverse = gives.get(from.slot());
    if (!reverse) {
      gives.set(from.slot(), std::vector<SubjectTerm>{});
      reverse = gives.get(from.slot());
    }
    reverse->push_back({subject, term.id});
  } else if (amount == 0) {
    row->parts.erase(found);
    auto *reverse = gives.get(from.slot());
    if (reverse)
      std::erase(*reverse, SubjectTerm{subject, term.id});
  } else
    found->amount = amount;
  const auto old = row->total;
  row->total = combined(*row, term.combine);
  effects.ordered.emplace_back(ContributionChange{{subject, term.id}});
  if (row->total != old)
    dirty(effects, subject, term.id);
  else
    effects.changed = true;
  return true;
}
void withdraw_contributor(SharedColumns &shared, GivesTo &gives, const Terms &terms, Handle from,
                          DeferredEffects &effects) {
  auto *reverse = gives.get(from.slot());
  if (!reverse)
    return;
  const auto refs = *reverse;
  for (const auto &ref : refs)
    set_contribution(shared, gives, terms.rows[ref.term], from, ref.subject, 0, effects);
  gives.erase(from.slot());
}
bool set_relation(Relation &relation, TermId term, Handle from, Handle to, bool present,
                  DeferredEffects &effects) {
  auto *out = relation.out.get(from.slot());
  const bool exists = out && std::find(out->begin(), out->end(), to) != out->end();
  if (exists == present)
    return false;
  if (present) {
    if (!out) {
      relation.out.set(from.slot(), std::vector<Handle>{});
      out = relation.out.get(from.slot());
    }
    out->push_back(to);
    auto *in = relation.in.get(to.slot());
    if (!in) {
      relation.in.set(to.slot(), std::vector<Handle>{});
      in = relation.in.get(to.slot());
    }
    in->push_back(from);
  } else {
    std::erase(*out, to);
    std::erase(*relation.in.get(to.slot()), from);
  }
  dirty(effects, from, term);
  dirty(effects, to, term);
  return true;
}
bool apply_derived_fact(RuntimeMeasurements &measurements, const DerivedFactInput &input,
                        DeferredEffects &effects) {
  auto it = measurements.values.find(input.source);
  if (it != measurements.values.end() && it->second == input.value)
    return false;
  measurements.values[input.source] = input.value;
  dirty(effects, input.source.subject, input.source.term);
  return true;
}
void retire_object(Properties &props, Processes &processes, ScriptStates &states,
                   RandomStates &random, Handle object, std::span<const ScriptId> scripts) {
  for (auto &column : props)
    column.erase(object.slot());
  for (auto &column : processes)
    column.erase(object.slot());
  for (auto script : scripts)
    states.erase({object, script});
  random.erase(object.bits);
}
std::size_t StateKeyHash::operator()(const StateKey &key) const {
  return std::visit([](const auto &v) { return std::hash<std::decay_t<decltype(v)>>{}(v); }, key) ^
         (key.index() * 0x9e3779b9);
}
std::size_t AllocationKeyHash::operator()(const AllocationKey &key) const {
  return FactKeyHash{}({key.subject, key.term}) ^
         std::hash<std::uint64_t>{}(key.contributor.bits +
                                    (key.supplier ? 0x9e3779b97f4a7c15ULL : 0));
}
double metered(const Meter &meter, Step now, double hz) {
  return meter.amount0 +
         meter.rate * (static_cast<double>(now) - static_cast<double>(meter.step0)) / hz;
}
namespace {
struct Demand {
  Handle from;
  double wanted, allocated = 0;
};
std::vector<Demand> divide(const Shared &supply, const Shared &demand, Combine combine,
                           const PriorityValues &priority) {
  std::vector<Demand> rows;
  for (auto &part : demand.parts)
    if (part.amount > 0)
      rows.push_back({part.from, part.amount, 0});
  double remaining = std::max(0.0, supply.total);
  if (combine == Combine::SharePriority) {
    auto rank = [&](Handle h) {
      auto it = priority.values.find(h.bits);
      return it == priority.values.end() ? 0 : it->second;
    };
    std::stable_sort(rows.begin(), rows.end(), [&](auto &a, auto &b) {
      const auto x = rank(a.from), y = rank(b.from);
      return x == y ? a.from < b.from : x > y;
    });
    for (auto &row : rows) {
      row.allocated = std::min(row.wanted, remaining);
      remaining -= row.allocated;
    }
  } else {
    std::vector<std::size_t> active(rows.size());
    std::iota(active.begin(), active.end(), 0);
    while (!active.empty() && remaining > 0) {
      const double share = remaining / static_cast<double>(active.size());
      std::vector<std::size_t> next;
      for (auto i : active) {
        auto &row = rows[i];
        const double amount = std::min(row.wanted - row.allocated, share);
        row.allocated += amount;
        remaining -= amount;
        if (row.allocated < row.wanted)
          next.push_back(i);
      }
      if (next.size() == active.size())
        break;
      active = std::move(next);
    }
  }
  return rows;
}
} // namespace
std::vector<AllocationKey> allocate(const Shared &supply, const Shared &demand, Combine combine,
                                    const PriorityValues &priority, Handle subject,
                                    TermId supply_term, TermId demand_term,
                                    amber::react::State &react_state, Step now, double hz) {

  auto &allocations = react_state.allocations;
  auto &meters = react_state.meters;

  std::vector<AllocationKey> changed;
  auto rows = divide(supply, demand, combine, priority);
  std::unordered_map<AllocationKey, double, AllocationKeyHash> next;
  double served = 0;
  for (auto &row : rows) {
    next[{subject, row.from, demand_term, false}] = row.allocated;
    served += row.allocated;
  }
  double offered = 0;
  for (auto &part : supply.parts)
    offered += std::max(0.0, part.amount);
  for (auto &part : supply.parts)
    next[{subject, part.from, supply_term, true}] =
        offered > 0 ? served * std::max(0.0, part.amount) / offered : 0;
  auto update = [&](AllocationKey key, double rate) {
    auto found = allocations.values.find(key);
    if (found != allocations.values.end() && found->second == rate)
      return;
    if (found == allocations.values.end() && rate == 0)
      return;
    auto [meter, fresh] = meters.try_emplace(key, Meter{0, now, rate});
    if (!fresh) {
      meter->second.amount0 = metered(meter->second, now, hz);
      meter->second.step0 = now;
      meter->second.rate = rate;
    }
    if (rate == 0) {
      allocations.values.erase(key);
    } else if (found == allocations.values.end()) {
      allocations.values.emplace(key, rate);
    } else {
      found->second = rate;
    }
    changed.push_back(key);
  };
  auto zero_missing = [&](TermId term, bool supplier) {
    std::vector<AllocationKey> keys;
    for (const auto &[key, value] : allocations.values) {
      (void)value;
      if (key.subject == subject && key.term == term && key.supplier == supplier &&
          !next.contains(key))
        keys.push_back(key);
    }
    for (auto key : keys)
      update(key, 0);
  };
  zero_missing(supply_term, true);
  zero_missing(demand_term, false);
  for (const auto &[key, rate] : next)
    update(key, rate);
  return changed;
}
double get_share(const AllocationRows &rows, Handle subject, TermId term, Handle contributor) {
  auto it = rows.values.find({subject, contributor, term, false});
  return it == rows.values.end() ? 0 : it->second;
}
double live_share(const ValueRead &values, Handle subject, TermId demand_term, Handle contributor,
                  Step now, double hz) {
  if (!values.identity.valid(subject))
    return 0;
  const auto &term = values.terms.rows.at(demand_term);
  const auto *supply = column(values.shared, term.paired, subject);
  const auto *demand = column(values.shared, demand_term, subject);
  if (!supply || !demand)
    return 0;
  PriorityValues priorities;
  if (term.combine == Combine::SharePriority && term.priority)
    for (const auto &part : demand->parts) {
      const auto answer = read(values, part.from, term.priority, now, hz);
      if (answer.present && answer.value.is_numeric())
        priorities.values.emplace(part.from.bits, answer.value.numeric());
    }
  for (const auto &row : divide(*supply, *demand, term.combine, priorities))
    if (row.from == contributor)
      return row.allocated;
  return 0;
}
void retire_allocations(AllocationRows &rows, MeterRows &meters, Handle object) {
  std::erase_if(rows.values, [&](const auto &row) {
    return row.first.subject == object || row.first.contributor == object;
  });
  // Earned integrals outlive withdrawal, but never either endpoint's lifetime.
  std::erase_if(meters, [&](const auto &row) {
    return row.first.subject == object || row.first.contributor == object;
  });
}

} // namespace amber::react
