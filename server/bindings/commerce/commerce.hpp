#pragma once

#include "bindings/allocation/allocation.hpp"
#include "bindings/geometry/geometry.hpp"
#include "bindings/goods.hpp"
#include "drive/drive.hpp"
#include "lifecycle/lifecycle.hpp"

namespace amber::bindings {
class SceneProjection;
}
namespace amber::bindings::commerce {
using Json = nlohmann::json;
struct Declarations {
  TermId pay = 0, balance = 0, inventory = 0, give = 0, offer = 0, offers = 0, accept = 0,
         cancel_offer = 0, trade_open = 0, trade_change = 0, trade_confirm = 0, trade_cancel = 0,
         place_existing = 0;
};
Declarations declare_commerce(react::Terms &, command::NativeActions &);
// Mechanical custody only. No money, permissions, text, scripts or catalog.
struct Custody {
  space::Space &space;
  IdentityRead identity;
  TransformWrite transforms;
  ParentWrite parents;
  SpaceFlags flags;
  space::PhysicalRelations &physical;
  const lifecycle::Parts &parts;
  const Relation &declared_parts;
  std::array<TermId, 4> relation_terms;
  SceneProjection &scene;
  PlotWrite membership;
  const perm::Plots &plots;
};
struct PaymentBinding {
  perm::Ledger &ledger;
  perm::Grants &grants;
  const perm::Players &players;
  IdentityRead identity;
  AuthorityRead authority;
  const perm::Groups &groups;
};
struct BalanceBinding {
  const perm::Ledger &ledger;
  const perm::Players &players;
  IdentityRead identity;
  AuthorityRead authority;
  const perm::Groups &groups;
};
struct InventoryBinding {
  const perm::Players &players;
};
struct GiveBinding {
  perm::Players &players;
  perm::Grants &grants;
  AuthorityWrite authority;
  PermissionFlags flags;
  perm::OwnerLimits &limits;
  Custody &custody;
  const DeliveryTargets &delivery;
  std::size_t mailbox_max;
};
struct OfferBinding {
  perm::Offers &offers;
  PermissionFlags flags;
  const perm::Players &players;
  const perm::Grants &grants;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  Custody &custody;
};
struct OffersBinding {
  const perm::Offers &offers;
  const perm::Players &players;
  IdentityRead identity;
  AuthorityRead authority;
};
struct OfferExpiryBinding {
  perm::Offers &offers;
  IdentityRead identity;
  PermissionFlags flags;
};
struct OfferExpired {
  perm::OfferId offer;
  PlayerId seller;
  Handle goods;
  std::vector<Handle> cleared;
};
std::vector<OfferExpired> advance_offer_expiry(OfferExpiryBinding &, Step, std::size_t max_entries);
struct AcceptBinding {
  perm::Offers &offers;
  perm::Ledger &ledger;
  perm::Grants &grants;
  perm::Players &players;
  AuthorityWrite authority;
  PermissionFlags flags;
  perm::OwnerLimits &limits;
  Custody &custody;
  const DeliveryTargets &delivery;
  std::size_t mailbox_max;
  const perm::Groups &groups;
};
struct TradeOpenBinding {
  perm::Trades &trades;
  const perm::Players &players;
  IdentityRead identity;
  const DeliveryTargets &delivery;
};
struct TradeChangeBinding {
  perm::Trades &trades;
  perm::Offers &offers;
  PermissionFlags flags;
  const perm::Players &players;
  const perm::Grants &grants;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  Custody &custody;
};
struct TradeConfirmBinding {
  perm::Trades &trades;
  perm::Offers &offers;
  perm::Ledger &ledger;
  perm::Grants &grants;
  perm::Players &players;
  AuthorityWrite authority;
  PermissionFlags flags;
  perm::OwnerLimits &limits;
  Custody &custody;
  const DeliveryTargets &delivery;
};
struct TradeCancelBinding {
  perm::Trades &trades;
  perm::Offers &offers;
  PermissionFlags flags;
  Custody &custody;
};
struct ExistingPlacementBinding {
  Custody &custody;
  TransformWrite transforms;
  PlotWrite membership;
  perm::Players &players;
  const perm::Grants &grants;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  const perm::Plots &plots;
  const GeometryAssets &geometry;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
  const react::ValueRead &values;
  PhysicsRoles roles;
  PhysicsDefaults defaults;
  AllocationReconciliation &allocation;
};
struct RetirementBinding {
  perm::Offers &offers;
  perm::Trades &trades;
  PermissionFlags flags;
  Custody &custody;
};
struct RebindOffersBinding {
  perm::Offers &offers;
  perm::Trades &trades;
  PermissionFlags flags;
  Custody &custody;
};
// Cancel affected offers/trades and return their goods through ordinary custody.
// This is an immediate effect, including when a later definition edit is rejected.
void rebind_offers(RebindOffersBinding &, std::span<const Handle> affected, const InvocationInput &,
                   react::DeferredEffects &);
struct MailboxDelivery {
  perm::Players &players;
  AuthorityRead authority;
  Custody &custody;
};
std::size_t deliver_mailbox(MailboxDelivery &, PlayerId, Handle authenticated_body, Step,
                            const record::Origin &, react::DeferredEffects &);
bool can_retire_goods(const RetirementBinding &, std::span<const Handle>, std::string &);
void retire_goods(RetirementBinding &, std::span<const Handle>, react::DeferredEffects &);
std::string validate_pay(const Json &);
std::string validate_balance(const Json &);
std::string validate_inventory(const Json &);
std::string validate_give(const Json &);
std::string validate_offer(const Json &);
std::string validate_offers(const Json &);
std::string validate_accept(const Json &);
std::string validate_cancel_offer(const Json &);
std::string validate_trade_open(const Json &);
std::string validate_trade_change(const Json &);
std::string validate_trade_confirm(const Json &);
std::string validate_trade_cancel(const Json &);
std::string validate_place_existing(const Json &);
drive::NativeCompletion pay(void *, const drive::NativeCall &);
drive::NativeCompletion balance(void *, const drive::NativeCall &);
drive::NativeCompletion inventory(void *, const drive::NativeCall &);
drive::NativeCompletion give(void *, const drive::NativeCall &);
drive::NativeCompletion offer(void *, const drive::NativeCall &);
drive::NativeCompletion offers(void *, const drive::NativeCall &);
drive::NativeCompletion accept(void *, const drive::NativeCall &);
drive::NativeCompletion cancel_offer(void *, const drive::NativeCall &);
drive::NativeCompletion trade_open(void *, const drive::NativeCall &);
drive::NativeCompletion trade_change(void *, const drive::NativeCall &);
drive::NativeCompletion trade_confirm(void *, const drive::NativeCall &);
drive::NativeCompletion trade_cancel(void *, const drive::NativeCall &);
drive::NativeCompletion place_existing(void *, const drive::NativeCall &);
} // namespace amber::bindings::commerce
