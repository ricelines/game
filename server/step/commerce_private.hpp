#pragma once
#include "step.hpp"

namespace amber::step::commerce_detail {
struct PlotChange {
  std::string_view op;
  perm::PlotFacts plot;
  PlayerId old_owner;
  float area, other_area = 0;
  std::optional<perm::PlotFacts> other{}, old{};
};
void plot_changed(NativeCompletion &, std::span<const perm::PlotHolder>, TermId, const NativeCall &,
                  const PlotChange &);
perm::Holder actor(const step::NativeCall &call);
PlayerId player(const step::NativeCall &call);
PlayerId player_id(const Json &input, const perm::Players &players);
perm::AccountTarget default_account(const step::NativeCall &call, IdentityRead identity,
                                    AuthorityRead authority, const perm::Players &players,
                                    const perm::Groups &groups);
Handle present(const DeliveryTargets &delivery, IdentityRead identity, PlayerId player);
step::NativeCompletion result(bool changed, Json extra = Json::object());
step::NativeCompletion denied(perm::Reason why, std::string need, std::string on,
                              PlayerId controller = 0);
step::NativeCompletion denied(std::string_view need, Json on, std::string_view why,
                              std::string_view fix);
void record(step::NativeCompletion &out, const step::NativeCall &call, std::string name,
            std::vector<Handle> objects, Json facts);
struct Goods {
  perm::Target root;
  std::vector<Handle> closure;
  std::vector<perm::Target> parts;
};
Goods goods(IdentityRead identity, const lifecycle::Parts &parts, const Relation &declared_parts,
            AuthorityRead authority, const perm::OwnerLimits &limits, const perm::Plots &plots,
            Handle h);
void check_custody(const space::Space &, IdentityRead, ParentRead, const lifecycle::Parts &,
                   const perm::CustodyChange &);

Json handles(std::span<const Handle> objects);
Json grants(std::span<const GrantId> ids);
void cleared_notice(step::NativeCompletion &out, const perm::Grants &rows,
                    std::span<const GrantId> revoked, PlayerId owner, Handle root,
                    std::span<const Handle> objects);
const perm::Offer &offer_row(const perm::Offers &offers, perm::OfferId id);
const perm::Trade &trade_row(const perm::Trades &trades, perm::TradeId id, PlayerId player);
} // namespace amber::step::commerce_detail
