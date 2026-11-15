#pragma once

#include "execution/execution.hpp"
#include "space/space.hpp"

#include <memory>

namespace amber::bindings {
struct CauseSummary {
  record::Origin origin;
  bool ambiguous = false;
};
class MechanicalCauses {
public:
  explicit MechanicalCauses(std::size_t authors_max = 8);
  ~MechanicalCauses();
  MechanicalCauses(const MechanicalCauses &) = delete;
  MechanicalCauses &operator=(const MechanicalCauses &) = delete;
  space::CauseToken intern(const record::Origin &, space::CauseToken previous = 0);
  space::CauseToken restore(CauseSummary);
  CauseSummary lookup(space::CauseToken) const;
  CauseSummary combine(const CauseSummary &, const CauseSummary &) const;
  void retain(std::span<const space::CauseToken>);

private:
  struct Data;
  std::unique_ptr<Data> data;
};
struct PhysicalRelationTerms {
  TermId contains, supports, attached_to, in_zone;
};
void relation_effects(react::DeferredEffects &, const PhysicalRelationTerms &,
                      std::span<const space::RelationChange>);
struct PhysicalEventTerms {
  TermId contact;
  PhysicalRelationTerms relations;
};
struct PhysicalPublication {
  MechanicalCauses &causes;
  step::WorkQueues &queues;
  record::Journal &journal;
  PhysicalEventTerms terms;
  std::size_t event_limit;
  double hz;
  CreditResolver credits;
};
void publish_physical(PhysicalPublication &, const space::StepResult &, Step);
void collect_physical_causes(MechanicalCauses &, const space::Space &);
} // namespace amber::bindings
