# Script account operations

`bindings/finance` provides balance, payment and issuance primitives to authored institutions. Each function has its own local references; no financial context can reach physics, transport, construction, profiles or other binding contexts.

Money is one integer wallet balance per player. Account arguments are `p:N` for registered players. An omitted balance account reads the installed object's current owner's player wallet. `self:pay(from, to, amount)` and `self:issue(to, amount)` accept nonnegative whole currency units. Objects and groups never hold money. Balances are inspectable game state. An invalid account is a script error before mutation.

Every mutation acts as the installed script-and-full-object holder, including inside an immediate body invocation. It never borrows the initiating player's wallet authority. `pay` uses the ordinary capped, expiring account grant. `issue` uses the separate issuance capability; owning an account or an institution name grants nothing. Both return `{ok, amount, reason}` from the actual permission/ledger result, and only a positive completed transfer marks the invocation changed. The underlying permission operation preflights both balances and the shared parent allowance before modifying any value.

Alternatives were implicit owner-wallet access and a native treasury scheduler. Either would bypass replaceable institutions and ordinary delegation. These primitives let a script select its own policy and payees while keeping the valuable exchange atomic. Public supply metering, procurement, rent and issuance schedules remain authored Luau.

Tests use real compiled Luau against actual grants and ledger rows. They cover one installation versus another instance's script identity, no inherited wallet authority, capped payment/issuance and revocation, live account validation and rejection of fractional amounts. Permission tests separately exercise parent counters, overflow and ownership transfer cleanup.
