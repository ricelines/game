#include "private.hpp"

namespace amber::bindings::commerce {
namespace {
void fields(const Json &row, std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional = {}) {
  if (!row.is_object())
    throw std::invalid_argument("expected an input record");
  for (auto key : required)
    if (!row.contains(key))
      throw std::invalid_argument("missing input: " + std::string(key));
  for (const auto &[key, value] : row.items()) {
    (void)value;
    if (std::find(required.begin(), required.end(), key) == required.end() &&
        std::find(optional.begin(), optional.end(), key) == optional.end())
      throw std::invalid_argument("unknown input: " + key);
  }
}
void id(const Json &row, std::string_view key) {
  if (!row.at(key).is_string() || row.at(key).get_ref<const std::string &>().empty())
    throw std::invalid_argument(std::string(key) + " must be an identifier string");
}
void amount(const Json &row, std::string_view key) {
  if (!row.at(key).is_number_integer() ||
      (row.at(key).is_number_unsigned() && row.at(key).get<std::uint64_t>() > INT64_MAX) ||
      row.at(key).get<std::int64_t>() < 0)
    throw std::invalid_argument(std::string(key) + " must be nonnegative whole currency units");
}
template <class Fn> std::string validate(Fn fn) {
  try {
    fn();
    return {};
  } catch (const std::exception &error) {
    return error.what();
  }
}
Json schema(Json properties, Json required, Json example) {
  return {{"type", "object"},
          {"properties", std::move(properties)},
          {"required", std::move(required)},
          {"additionalProperties", false},
          {"examples", Json::array({std::move(example)})}};
}
} // namespace
std::string validate_pay(const Json &row) {
  return validate([&] {
    fields(row, {"to", "amount"}, {"from"});
    id(row, "to");
    amount(row, "amount");
    if (row.contains("from"))
      id(row, "from");
  });
}
std::string validate_balance(const Json &row) {
  return validate([&] {
    fields(row, {}, {"account"});
    if (row.contains("account"))
      id(row, "account");
  });
}
std::string validate_inventory(const Json &row) {
  return validate([&] { fields(row, {}); });
}
std::string validate_give(const Json &row) {
  return validate([&] {
    fields(row, {"recipient"});
    id(row, "recipient");
  });
}
std::string validate_offer(const Json &row) {
  return validate([&] {
    fields(row, {"price"}, {"buyer", "expires_s"});
    amount(row, "price");
    if (row.contains("buyer"))
      id(row, "buyer");
    if (row.contains("expires_s") &&
        (!row["expires_s"].is_number() || !std::isfinite(row["expires_s"].get<double>()) ||
         row["expires_s"].get<double>() < 0))
      throw std::invalid_argument("expires_s must be finite nonnegative seconds");
  });
}
std::string validate_offers(const Json &row) {
  return validate_inventory(row);
}
std::string validate_accept(const Json &row) {
  return validate([&] {
    fields(row, {"offer"}, {"payer", "beneficiary"});
    id(row, "offer");
    if (row.contains("payer"))
      id(row, "payer");
    if (row.contains("beneficiary"))
      id(row, "beneficiary");
  });
}
std::string validate_cancel_offer(const Json &row) {
  return validate([&] {
    fields(row, {"offer"});
    id(row, "offer");
  });
}
std::string validate_trade_open(const Json &row) {
  return validate([&] {
    fields(row, {"other"});
    id(row, "other");
  });
}
std::string validate_trade_change(const Json &row) {
  return validate([&] {
    fields(row, {"trade", "money"}, {"goods"});
    id(row, "trade");
    amount(row, "money");
    if (row.contains("goods") && !row["goods"].is_null())
      id(row, "goods");
  });
}
std::string validate_trade_confirm(const Json &row) {
  return validate([&] {
    fields(row, {"trade"});
    id(row, "trade");
  });
}
std::string validate_trade_cancel(const Json &row) {
  return validate_trade_confirm(row);
}
std::string validate_place_existing(const Json &row) {
  return validate([&] {
    fields(row, {"position"}, {"rotation"});
    for (const auto &[key, n] : {std::pair{"position", 3}, {"rotation", 4}}) {
      if (!row.contains(key))
        continue;
      const auto &values = row.at(key);
      if (!values.is_array() || values.size() != std::size_t(n))
        throw std::invalid_argument(std::string(key) + " has wrong vector length");
      double norm = 0;
      for (const auto &v : values) {
        if (!v.is_number() || !std::isfinite(v.get<float>()))
          throw std::invalid_argument(std::string(key) + " must be finite");
        norm += v.get<double>() * v.get<double>();
      }
      if (n == 4 && (!(norm > 0) || !std::isfinite(norm)))
        throw std::invalid_argument("rotation must be a nonzero quaternion [x,y,z,w]");
    }
  });
}
Declarations declare_commerce(react::Terms &terms, command::NativeActions &native) {
  Declarations ids;
  const Json str = {{"type", "string"}};
  const Json money = {{"type", "integer"}, {"minimum", 0}, {"maximum", INT64_MAX}};
  auto add = [&](std::string name, std::string description, std::string right, Json input) {
    return command::declare_native(native, terms, std::move(name), std::move(description),
                                   std::move(right), false, std::move(input));
  };
  ids.pay = add("engine.pay",
                "Transfer whole currency units between existing player wallets. Scripts need an "
                "explicit capped Spend grant on the payer wallet.",
                "",
                schema({{"to", str}, {"amount", money}, {"from", str}}, {"to", "amount"},
                       {{"to", "p:PLAYER_ID"}, {"amount", 1}}));
  ids.balance =
      add("engine.balance",
          "Read a player wallet. Omitted account selects the player caller, or the installed "
          "script object's current owner; selection grants no spending authority.",
          "", schema({{"account", str}}, Json::array(), Json::object()));
  ids.inventory =
      add("engine.inventory", "List your inventory and mailbox roots with full object identities.",
          "", schema(Json::object(), Json::array(), Json::object()));
  ids.give =
      add("engine.give",
          "Give the target complete declared item to a player; absent players receive mailbox "
          "delivery. Transfer permission and capacity are checked before ownership changes.",
          "transfer", schema({{"recipient", str}}, {"recipient"}, {{"recipient", "p:PLAYER_ID"}}));
  ids.offer = add(
      "engine.offer",
      "Offer the placed target complete item for sale. Income goes to its current owner wallet; "
      "escrow stays visible. Optional buyer restricts acceptance.",
      "transfer",
      schema(
          {{"price", money}, {"buyer", str}, {"expires_s", {{"type", "number"}, {"minimum", 0}}}},
          {"price"}, {{"price", 1}}));
  ids.offers = add("engine.offers",
                   "List live ordinary offers available to you or sold by you; an optional target "
                   "narrows the goods.",
                   "", schema(Json::object(), Json::array(), Json::object()));
  ids.accept = add("engine.accept",
                   "Accept an ordinary offer, paying the seller and receiving the complete item. "
                   "Scripts must name a beneficiary and hold payer-wallet Spend permission.",
                   "",
                   schema({{"offer", str}, {"payer", str}, {"beneficiary", str}}, {"offer"},
                          {{"offer", "offer:OFFER_ID"}}));
  ids.cancel_offer = add(
      "engine.cancel_offer",
      "Cancel an ordinary offer using current Transfer permission and release its placed escrow.",
      "", schema({{"offer", str}}, {"offer"}, {{"offer", "offer:OFFER_ID"}}));
  ids.trade_open =
      add("engine.trade_open", "Open a direct trade with another currently present player.", "",
          schema({{"other", str}}, {"other"}, {{"other", "p:PLAYER_ID"}}));
  ids.trade_change =
      add("engine.trade_change",
          "Set your direct-trade side's money and optional complete item. Changing either side "
          "clears both confirmations.",
          "",
          schema({{"trade", str}, {"money", money}, {"goods", {{"type", {"string", "null"}}}}},
                 {"trade", "money"}, {{"trade", "trade:TRADE_ID"}, {"money", 0}}));
  ids.trade_confirm = add("engine.trade_confirm",
                          "Confirm your direct-trade side. The second confirmation rechecks both "
                          "present players, goods and wallets before one complete exchange.",
                          "", schema({{"trade", str}}, {"trade"}, {{"trade", "trade:TRADE_ID"}}));
  ids.trade_cancel =
      add("engine.trade_cancel",
          "Cancel a direct trade and restore each item's recorded world or inventory custody.", "",
          schema({{"trade", str}}, {"trade"}, {{"trade", "trade:TRADE_ID"}}));
  const Json pos = {
      {"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}};
  const Json rot = {
      {"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}};
  ids.place_existing =
      add("engine.place_existing",
          "Place your existing inventory item at position [x,y,z] and optional quaternion rotation "
          "[x,y,z,w]. Requires Move and destination Build; preserves identities and does not "
          "fabricate again.",
          "move",
          schema({{"position", pos}, {"rotation", rot}}, {"position"}, {{"position", {0, 0, 0}}}));
  return ids;
}
} // namespace amber::bindings::commerce
