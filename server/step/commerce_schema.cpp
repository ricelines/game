#include "catalog/catalog.hpp"
#include "commerce_private.hpp"

namespace amber::step {
namespace {
Json schema(Json properties, Json required, Json example) {
  return {{"type", "object"},
          {"properties", std::move(properties)},
          {"required", std::move(required)},
          {"additionalProperties", false},
          {"examples", Json::array({std::move(example)})}};
}
} // namespace
CommerceDeclarations declare_commerce(react::Terms &terms, react::NativeActions &native) {
  CommerceDeclarations ids;
  const Json str = {{"type", "string"}};
  const Json money = {{"type", "integer"}, {"minimum", 0}, {"maximum", INT64_MAX}};
  auto add = [&](react::NativeOperation operation, std::string name, std::string description,
                 std::string right, Json input) {
    return catalog::declare_native(native, terms, std::move(name), std::move(description),
                                   std::move(right), false, std::move(input), operation);
  };
  ids.notify = add(react::ExchangeAction::Notify, "engine.notify",
                   "Send a notice to a player who granted this script authority.", "",
                   schema({{"player", str}, {"text", str}}, {"player", "text"},
                          {{"player", "p:PLAYER_ID"}, {"text", "Your lot needs tax credit."}}));
  ids.pay =
      add(react::ExchangeAction::Pay, "engine.pay",
          "Transfer whole currency units between player, object or group accounts. Scripts need an "
          "explicit capped Spend grant on the payer account.",
          "",
          schema({{"to", str}, {"amount", money}, {"from", str}}, {"to", "amount"},
                 {{"to", "p:PLAYER_ID"}, {"amount", 1}}));
  ids.balance =
      add(react::ExchangeAction::Balance, "engine.balance",
          "Read an account balance. Omitted account selects the player caller, or the installed "
          "script object itself; selection grants no spending authority.",
          "", schema({{"account", str}}, Json::array(), Json::object()));
  ids.inventory = add(react::ExchangeAction::Inventory, "engine.inventory",
                      "List your inventory and mailbox roots with full object identities.", "",
                      schema(Json::object(), Json::array(), Json::object()));
  ids.give =
      add(react::ExchangeAction::Give, "engine.give",
          "Give the target complete declared item to a player; absent players receive mailbox "
          "delivery. Transfer permission and capacity are checked before ownership changes.",
          "transfer", schema({{"recipient", str}}, {"recipient"}, {{"recipient", "p:PLAYER_ID"}}));
  ids.offer = add(
      react::ExchangeAction::Offer, "engine.offer",
      "Offer the placed target complete item for sale. Income goes to its current owner wallet; "
      "escrow stays visible. Optional buyer restricts acceptance.",
      "transfer",
      schema(
          {{"price", money}, {"buyer", str}, {"expires_s", {{"type", "number"}, {"minimum", 0}}}},
          {"price"}, {{"price", 1}}));
  const Json plot = {{"type", "integer"}, {"minimum", 1}, {"maximum", UINT32_MAX}};
  ids.abandon_plot = add(react::ExchangeAction::AbandonPlot, "engine.abandon_plot",
                         "Return a lot to the pool and return its contents to their owners.", "",
                         schema({{"plot", plot}}, {"plot"}, {{"plot", 1}}));
  auto split = schema({{"plot", plot}, {"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}},
                      {"plot"}, {{"plot", 1}, {"x", 16}});
  split["oneOf"] = Json::array({Json{{"required", {"x"}}}, Json{{"required", {"y"}}}});
  ids.split_plot = add(react::ExchangeAction::SplitPlot, "engine.split_plot",
                       "Split a lot at one whole-metre x or y coordinate.", "", std::move(split));
  ids.join_plots = add(
      react::ExchangeAction::JoinPlots, "engine.join_plots",
      "Join two same-owner lots sharing a whole edge into one rectangle.", "",
      schema({{"plots", {{"type", "array"}, {"items", plot}, {"minItems", 2}, {"maxItems", 2}}}},
             {"plots"}, {{"plots", {1, 2}}}));
  ids.return_goods = add(
      react::ExchangeAction::ReturnGoods, "engine.return",
      "Return another owner's complete item from your land. A live Build grant protects its owner.",
      "", schema(Json::object(), Json::array(), Json::object()));
  ids.offer_plot =
      add(react::ExchangeAction::OfferPlot, "engine.offer_plot",
          "Offer a lot for sale to anyone or a named buyer. Requires Transfer on its land.", "",
          schema({{"plot", {{"type", "integer"}, {"minimum", 1}, {"maximum", UINT32_MAX}}},
                  {"price", money},
                  {"buyer", str},
                  {"expires_s", {{"type", "number"}, {"minimum", 0}}}},
                 {"plot", "price"}, {{"plot", 1}, {"price", 1}}));
  ids.set_offer_price = add(react::ExchangeAction::SetOfferPrice, "engine.set_offer_price",
                            "Set an ordinary offer's price as its maker or seller.", "",
                            schema({{"offer", str}, {"price", money}}, {"offer", "price"},
                                   {{"offer", "offer:1"}, {"price", 1}}));
  ids.offers = add(react::ExchangeAction::Offers, "engine.offers",
                   "List live ordinary offers available to you or sold by you; an optional target "
                   "narrows the goods.",
                   "", schema(Json::object(), Json::array(), Json::object()));
  ids.accept =
      add(react::ExchangeAction::Accept, "engine.accept",
          "Accept a goods or plot offer up to max_price, paying the seller. "
          "Scripts must name a beneficiary and hold payer-wallet Spend permission.",
          "",
          schema({{"offer", str}, {"max_price", money}, {"payer", str}, {"beneficiary", str}},
                 {"offer", "max_price"}, {{"offer", "offer:OFFER_ID"}, {"max_price", 1}}));
  ids.cancel_offer = add(
      react::ExchangeAction::CancelOffer, "engine.cancel_offer",
      "Cancel an ordinary offer using current Transfer permission and release its placed escrow.",
      "", schema({{"offer", str}}, {"offer"}, {{"offer", "offer:OFFER_ID"}}));
  ids.trade_open = add(react::ExchangeAction::TradeOpen, "engine.trade_open",
                       "Open a direct trade with another currently present player.", "",
                       schema({{"other", str}}, {"other"}, {{"other", "p:PLAYER_ID"}}));
  ids.trade_change =
      add(react::ExchangeAction::TradeChange, "engine.trade_change",
          "Set your direct-trade side's money and optional complete item. Changing either side "
          "clears both confirmations.",
          "",
          schema({{"trade", str}, {"money", money}, {"goods", {{"type", {"string", "null"}}}}},
                 {"trade", "money"}, {{"trade", "trade:TRADE_ID"}, {"money", 0}}));
  ids.trade_confirm = add(react::ExchangeAction::TradeConfirm, "engine.trade_confirm",
                          "Confirm your direct-trade side. The second confirmation rechecks both "
                          "present players, goods and wallets before one complete exchange.",
                          "", schema({{"trade", str}}, {"trade"}, {{"trade", "trade:TRADE_ID"}}));
  ids.trade_cancel =
      add(react::ExchangeAction::TradeCancel, "engine.trade_cancel",
          "Cancel a direct trade and restore each item's recorded world or inventory custody.", "",
          schema({{"trade", str}}, {"trade"}, {{"trade", "trade:TRADE_ID"}}));
  const Json pos = {
      {"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}};
  const Json rot = {
      {"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}};
  ids.place_existing =
      add(react::ConstructionAction::PlaceExisting, "engine.place_existing",
          "Place your existing inventory item at position [x,y,z] and optional quaternion rotation "
          "[x,y,z,w]. Requires Move and destination Build; preserves identities and does not "
          "fabricate again.",
          "move",
          schema({{"position", pos}, {"rotation", rot}}, {"position"}, {{"position", {0, 0, 0}}}));
  for (const auto id : {ids.offer_plot, ids.split_plot, ids.join_plots, ids.abandon_plot})
    native.rows.at(id).space = "land";
  return ids;
}
} // namespace amber::step
