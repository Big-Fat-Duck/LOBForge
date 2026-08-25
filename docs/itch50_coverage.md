# TotalView-ITCH 5.0 application-message coverage

Normative baseline: [Nasdaq TotalView-ITCH 5.0, June 11 2026](https://assets.ctfassets.net/mx0rke14e5yt/5Uz6MGJxbo4wRPou8KveFs/4d76437c8e57694acee9d767587a8dfa/6-11-26_TVITCH_5.0_1.pdf), including the 2026 listing-market update (`F` = Texas Exchange), the earlier `M` listing-market value, and IPO flag `Z`. Each representation below contains the common stock-locate, tracking-number, and 48-bit timestamp fields plus every listed type-specific field. “Golden” means an independently specified literal-hex payload; “negative” means every truncated prefix, wrong length, and applicable enum validation.

| Type | Meaning | Bytes | Typed C++ representation | Decoder | Factual/session effect | Golden fixture | Negative tests | Status |
|---|---|---:|---|---|---|---|---|---|
| `S` | System Event | 12 | `SystemEvent` | explicit BE/common + event code | lifecycle `O,S,Q,M,E,C` | `S` hex | truncation/enum/length | Complete |
| `R` | Stock Directory | 39 | `StockDirectory` | all 15 directory fields | day-local locate/symbol reference | `R` hex | truncation/enum/length/conflict | Complete |
| `H` | Stock Trading Action | 25 | `StockTradingAction` | stock/state/reserved/reason | latest typed stock action | `H` hex | truncation/enum/length/locate | Complete |
| `Y` | Reg SHO Restriction | 20 | `RegShoRestriction` | stock/action | latest typed restriction | `Y` hex | truncation/enum/length/locate | Complete |
| `L` | Market Participant Position | 26 | `MarketParticipantPosition` | MPID/stock/three status fields | latest MPID-symbol position | `L` hex | truncation/enum/length/locate | Complete |
| `V` | MWCB Decline Levels | 35 | `MwcbDeclineLevels` | three BE Price(8) fields | latest global levels | `V` hex | truncation/length/locate | Complete |
| `W` | MWCB Status | 12 | `MwcbStatus` | breached-level enum | latest global status | `W` hex | truncation/enum/length/locate | Complete |
| `K` | IPO Quoting Period Update | 28 | `IpoQuotingPeriodUpdate` | stock/time/qualifier/Price(4) | latest typed stock update | `K` hex | truncation/enum/length/locate | Complete |
| `J` | LULD Auction Collar | 35 | `LuldAuctionCollar` | stock/three Price(4)/extension | latest typed collar | `J` hex | truncation/length/locate | Complete |
| `h` | Operational Halt | 21 | `OperationalHalt` | stock/market/action | latest market-stock halt | `h` hex | truncation/enum/length/locate | Complete |
| `A` | Add Order, no MPID | 36 | `AddOrder` | reference/side/shares/stock/price | add displayed FIFO order | `A` hex | truncation/enum/length/duplicate/zero | Complete |
| `F` | Add Order, with MPID | 40 | `AddOrderMpid` | add fields + attribution | add attributed displayed order | `F` hex | truncation/enum/length/duplicate/zero | Complete |
| `E` | Order Executed | 31 | `OrderExecuted` | reference/shares/match | reduce display; printable ledger trade | `E` hex | truncation/length/unknown/overrun/locate | Complete |
| `C` | Order Executed With Price | 36 | `OrderExecutedWithPrice` | execution fields/printable/price | reduce at display price; ledger execution price | `C` hex | truncation/enum/length/unknown/overrun | Complete |
| `X` | Order Cancel | 23 | `OrderCancel` | reference/cancelled shares | reduce/remove display | `X` hex | truncation/length/unknown/overrun/zero | Complete |
| `D` | Order Delete | 19 | `OrderDelete` | reference | remove all display, including after `E` session event | `D` hex | truncation/length/unknown/locate | Complete |
| `U` | Order Replace | 35 | `OrderReplace` | old/new refs/shares/price | atomic old remove + new FIFO append | `U` hex | truncation/length/unknown/duplicate/zero | Complete |
| `P` | Non-Cross Trade | 44 | `NonCrossTrade` | ref/side/shares/stock/price/match | printable trade ledger only | `P` hex | truncation/enum/length/locate | Complete |
| `Q` | Cross Trade | 40 | `CrossTrade` | 64-bit shares/stock/price/match/cross type | printable cross ledger only | `Q` hex | truncation/enum/length/locate | Complete |
| `B` | Broken Trade/Execution | 19 | `BrokenTrade` | match number | mark ledger execution broken; no depth, allowed late | `B` hex | truncation/length/unknown match | Complete |
| `I` | NOII | 50 | `Noii` | paired/imbalance shares, direction, stock, prices, cross/variation | latest typed imbalance state | `I` hex | truncation/enum/length/locate | Complete |
| `N` | Retail Price Improvement | 20 | `RetailPriceImprovement` | stock/interest flag | latest typed stock indicator | `N` hex | truncation/enum/length/locate | Complete |
| `O` | DLCR Price Discovery | 48 | `DlcrPriceDiscovery` | eligibility, five prices, near-execution time | latest typed stock discovery state | `O` hex | truncation/enum/length/locate | Complete |

`H` and lowercase `h` are separate. Type `S` event code `O` is Start of Messages; message type `O` is DLCR price discovery. A row becomes “Complete” only when decoding, state effect, golden fixture, and negative tests all pass; no row is satisfied by opaque skipping.
