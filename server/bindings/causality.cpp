#include "causality.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace amber::bindings {
struct MechanicalCauses::Data {
  struct Entry {
    CauseSummary summary;
    space::CauseToken previous = 0;
    bool pending = false;
  };
  std::size_t authors_max;
  space::CauseToken next = 1;
  std::unordered_map<space::CauseToken, Entry> entries;
  std::unordered_map<std::uint64_t, space::CauseToken> interned;
};
MechanicalCauses::MechanicalCauses(std::size_t authors_max) : data(std::make_unique<Data>()) {
  data->authors_max = authors_max;
}
MechanicalCauses::~MechanicalCauses() = default;
space::CauseToken MechanicalCauses::intern(const record::Origin &origin) {
  if (!origin.cause && !origin.root)
    return 0;
  if (auto found = data->interned.find(origin.cause); found != data->interned.end())
    return found->second;
  auto token = restore({origin, false});
  data->interned[origin.cause] = token;
  return token;
}
space::CauseToken MechanicalCauses::restore(CauseSummary summary) {
  auto token = data->next++;
  data->entries.emplace(token, Data::Entry{std::move(summary)});
  return token;
}
space::CauseToken MechanicalCauses::begin(space::CauseToken previous, bool additive) {
  auto token = data->next++;
  data->entries.emplace(token, Data::Entry{{}, additive ? previous : 0, true});
  return token;
}
void MechanicalCauses::finalize(space::CauseToken token, const record::Origin &origin) {
  auto &entry = data->entries.at(token);
  entry.summary = combine(lookup(entry.previous), {origin, false});
  entry.previous = 0;
  entry.pending = false;
}
void MechanicalCauses::abandon(space::CauseToken token) {
  data->entries.erase(token);
}
CauseSummary MechanicalCauses::lookup(space::CauseToken token) const {
  return token ? data->entries.at(token).summary : CauseSummary{};
}
CauseSummary MechanicalCauses::combine(const CauseSummary &a, const CauseSummary &b) const {
  const auto empty = [](const CauseSummary &v) {
    return !v.ambiguous && !v.origin.cause && !v.origin.root;
  };
  if (empty(a))
    return b;
  if (empty(b))
    return a;
  CauseSummary result = a.origin.cause > b.origin.cause ? a : b;
  result.ambiguous = a.ambiguous || b.ambiguous || a.origin.root != b.origin.root;
  result.origin.authors = a.origin.authors;
  result.origin.authors.insert(result.origin.authors.end(), b.origin.authors.begin(),
                               b.origin.authors.end());
  auto &authors = result.origin.authors;
  std::sort(authors.begin(), authors.end());
  authors.erase(std::unique(authors.begin(), authors.end()), authors.end());
  result.origin.more = a.origin.more || b.origin.more || authors.size() > data->authors_max;
  if (authors.size() > data->authors_max)
    authors.resize(data->authors_max);
  if (a.origin.oldest && b.origin.oldest)
    result.origin.oldest = std::min(*a.origin.oldest, *b.origin.oldest);
  else
    result.origin.oldest = a.origin.oldest ? a.origin.oldest : b.origin.oldest;
  if (result.ambiguous)
    result.origin.cause = result.origin.root = 0;
  return result;
}
void MechanicalCauses::retain(std::span<const space::CauseToken> tokens) {
  std::unordered_set<space::CauseToken> live(tokens.begin(), tokens.end());
  std::erase_if(data->entries,
                [&](const auto &row) { return !row.second.pending && !live.contains(row.first); });
  std::erase_if(data->interned,
                [&](const auto &row) { return !data->entries.contains(row.second); });
}
react::CausalReceipt motion_receipt(MechanicalCauses &causes, space::CauseToken token) {
  return {&causes, token, [](void *owner, std::uint64_t receipt, const record::Origin &origin) {
            static_cast<MechanicalCauses *>(owner)->finalize(receipt, origin);
          }};
}
namespace {
TermId relation_term(const PhysicalRelationTerms &terms, space::RelationKind kind) {
  switch (kind) {
  case space::RelationKind::Contains:
    return terms.contains;
  case space::RelationKind::Supports:
    return terms.supports;
  case space::RelationKind::AttachedTo:
    return terms.attached_to;
  case space::RelationKind::InZone:
    return terms.in_zone;
  }
  std::terminate();
}
record::Origin published_origin(PhysicalPublication &b, const CauseSummary &summary, Step step,
                                std::span<const Handle> things) {
  if (!summary.ambiguous)
    return summary.origin;
  record::Event event;
  event.step = step;
  event.kind = record::Kind::System;
  event.name = "physical_interaction";
  event.result = record::Result::NoEffect;
  event.changed = false;
  event.things.assign(things.begin(), things.end());
  event.detail["causal_ambiguity"] = true;
  if (b.credits.resolve)
    event.credits = b.credits.resolve(b.credits.context, things, 0, b.hz);
  return b.journal.append(std::move(event), summary.origin);
}
} // namespace
void relation_effects(react::DeferredEffects &effects, const PhysicalRelationTerms &terms,
                      std::span<const space::RelationChange> changes) {
  for (const auto &change : changes) {
    const auto term = relation_term(terms, change.kind);
    effects.ordered.emplace_back(step::WatchWork{{change.from, term}, {}});
    effects.ordered.emplace_back(step::WatchWork{{change.to, term}, {}});
  }
}
void publish_physical(PhysicalPublication &b, const space::StepResult &physics, Step now) {
  for (const auto &contact : physics.contacts) {
    const auto summary = b.causes.combine(b.causes.lookup(contact.causes.first),
                                          b.causes.lookup(contact.causes.second));
    const std::array<Handle, 2> things{contact.first, contact.second};
    const auto origin = published_origin(b, summary, now, things);
    for (const auto &[receiver, sender, normal] :
         {std::tuple{contact.first, contact.second, contact.normal},
          std::tuple{contact.second, contact.first, contact.normal * -1}})
      step::enqueue_event(b.queues,
                          {receiver, sender, b.terms.contact,
                           std::vector<EventField>{{"point", contact.point},
                                                   {"normal", normal},
                                                   {"started", Value(contact.started)}},
                           origin},
                          b.event_limit);
  }
  std::map<std::pair<std::uint64_t, TermId>, CauseSummary> facts;
  for (const auto &change : physics.relations) {
    const auto summary = b.causes.combine(b.causes.lookup(change.causes.first),
                                          b.causes.lookup(change.causes.second));
    const auto term = relation_term(b.terms.relations, change.kind);
    for (auto object : {change.from, change.to}) {
      auto &combined = facts[{object.bits, term}];
      combined = b.causes.combine(combined, summary);
    }
  }
  for (const auto &[key, summary] : facts) {
    const Handle object{key.first};
    const auto origin = published_origin(b, summary, now, std::span(&object, 1));
    step::enqueue_watch(b.queues, {{object, key.second}, origin});
  }
}
void collect_physical_causes(MechanicalCauses &causes, const space::Space &space) {
  std::vector<space::CauseToken> tokens;
  space::active_cause_tokens(space, tokens);
  causes.retain(tokens);
}
} // namespace amber::bindings
