#include "store/store.hpp"

#include <type_traits>

template <class T>
concept HasLedger = requires(T t) { t.ledger; };
template <class T>
concept HasOwner = requires(T t) { t.owner; };
template <class T>
concept CanWritePlot = requires(T t) { t.plot[0] = amber::PlotId{1}; };
template <class T>
concept CanWriteFlags = requires(T t) { t.flags[0] = std::uint32_t{1}; };
template <class T>
concept CanWriteIdentity = requires(T t) { t.model[0] = amber::ModelId{1}; };
template <class T>
concept CanWriteTransform = requires(T t) { t.position[0] = amber::Vec3{}; };

static_assert(!HasLedger<amber::IdentityRead>);
static_assert(!HasOwner<amber::IdentityRead>);
static_assert(!HasOwner<amber::TransformWrite>);
static_assert(!CanWritePlot<amber::AuthorityWrite>);
static_assert(!CanWriteFlags<amber::IdentityRead>);
static_assert(!CanWriteIdentity<amber::IdentityRead>);
static_assert(!CanWriteTransform<amber::TransformRead>);
static_assert(CanWriteTransform<amber::TransformWrite>);
static_assert(!std::is_convertible_v<amber::Objects &, amber::TransformWrite>);

int main() {}
