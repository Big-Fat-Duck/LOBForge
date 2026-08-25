# `lobforge.book_event` version 1

`book_event/v1` is the auditable Round 3 research boundary between the authoritative C++ ITCH
decoder/factual reconstructor and Python. It is newline-delimited UTF-8 JSON with one JSON object
and one LF byte per line. It is not presented as a production low-latency transport.

## Invocation and stream lifecycle

```sh
lobforge_replay --input session.itch --session-date 2026-08-24 --strict \
  --research-format book-event-v1 --depth 10 --output -
```

`--session-date` is mandatory because an ITCH timestamp has no calendar date. `--symbol` may be
repeated. `--depth` is in `[1,10]` and defaults to 10. `--output -` is mandatory for this format.
NDJSON is the only stdout content; diagnostics and errors use stderr. Strict/permissive behavior is
the Round 2 behavior. A successful stream is exactly `header`, zero or more `book_event` records,
then `summary`. There is no record after `summary`.

The exporter uses locale-independent integer formatting, fixed key order and no wall clock, random
ID, address, temporary path, host path, or process-specific value. A selected mutation sequence is
numbered contiguously from one after symbol filtering. `source_offset` preserves its location in the
unfiltered input.

## Header

Keys occur in this exact order:

| Field | JSON type | Null | Meaning |
|---|---:|---:|---|
| `record_type` | string | no | Literal `header` |
| `schema` | string | no | Literal `lobforge.book_event` |
| `version` | integer | no | Literal `1` |
| `session_date` | `YYYY-MM-DD` string | no | User-supplied trading date |
| `price_scale` | integer | no | Literal `10000`; prices are Price(4) integers |
| `timestamp_unit` | string | no | Literal `ns_since_midnight` |
| `depth` | integer | no | Requested depth, 1 through 10 |
| `source_size` | integer | no | Input file bytes |

## Book event

Keys occur in the order shown. Unsigned ranges are validated before Arrow conversion. JSON has no
unsigned primitive, so “unsigned” below is a semantic range constraint.

| Field | Type | Null | Meaning |
|---|---:|---:|---|
| `record_type` | string | no | Literal `book_event` |
| `session_date` | string | no | Same date as the header |
| `sequence` | unsigned 64-bit integer | no | Contiguous selected-mutation ordinal |
| `source_offset` | unsigned 64-bit integer | no | Byte offset of the u16 envelope in the source |
| `timestamp_ns` | integer `[0,86400000000000)` | no | Feed nanoseconds since midnight |
| `stock_locate` | unsigned 16-bit integer, nonzero | no | ITCH stock-locate code |
| `symbol` | nonempty string | no | Trimmed stock symbol from the factual state |
| `message_type` | string | no | One of `A F E C X D U` |
| `action` | string | no | `add`, `execute`, `execute_with_price`, `cancel`, `delete`, or `replace` |
| `side` | string | no | `B` or `S` |
| `order_ref` | unsigned 64-bit integer, nonzero | no | Affected old/current order reference |
| `new_order_ref` | unsigned 64-bit integer | yes | Non-null only for `U` |
| `event_qty` | unsigned 64-bit integer, nonzero | no | Added/replaced/executed/cancelled/deleted display quantity |
| `display_price4` | positive integer | no | Original displayed order price; for `C`, not the execution price |
| `execution_price4` | positive integer | yes | Non-null only for `C` |
| `match_number` | unsigned 64-bit integer | yes | Non-null only for `E` and `C` |
| `session_state` | string | no | C++ factual system-event phase |
| `trading_state` | string | yes | Latest `H/P/Q/T` stock trading action, or null if not yet known |
| `two_sided` | Boolean | no | Both exported depth arrays are nonempty |
| `locked` | Boolean | no | Two-sided and best bid equals best ask |
| `crossed` | Boolean | no | Two-sided and best bid exceeds best ask |
| `bids` | array of `[price4, aggregate_qty]` | no | At most `depth`, strictly descending best-to-worst |
| `asks` | array of `[price4, aggregate_qty]` | no | At most `depth`, strictly ascending best-to-worst |

Every successfully applied `A/F/E/C/X/D/U` produces exactly one post-event record. A replace (`U`)
is atomic and never exposes a remove/add intermediate state. An execution with price (`C`) keeps
`display_price4` and `execution_price4` separate. `P/Q/B` and all reference/session messages may
change factual non-book state but do not produce displayed-book mutation rows. All integers remain
integers at this boundary.

## Summary

Key order is `record_type`, `schema`, `version`, `input_bytes`, `records_seen`, `records_decoded`,
`records_applied`, `records_output`, `records_skipped`, `errors`, `factual_book_digest`.
Counters are unsigned integers. `errors` is the total deterministic diagnostic count. The digest is
16 lowercase hexadecimal digits over the final C++ factual book state; it is not a cryptographic
source digest. Python separately records the input SHA-256.

## Version and failure behavior

Consumers must reject an unknown schema name or version; version 1 is not interpreted by guessing
new fields. A future compatible format uses a new integer version and documentation. Python rejects
invalid JSON, an unterminated line, missing/extra/reordered fields in its scalar oracle, wrong
types/ranges/nullability, non-monotone timestamps/offsets, non-contiguous selected sequence,
inconsistent flags/counters, a missing summary, or a record after summary. It never skips a bad
line. C++ usage errors return 2, source/output errors 3, replay failures 4, and invariant failures 5.
The Python public exit categories are documented in `lobforge_research.errors`.

## Example

```json
{"record_type":"header","schema":"lobforge.book_event","version":1,"session_date":"2026-08-24","price_scale":10000,"timestamp_unit":"ns_since_midnight","depth":1,"source_size":810}
{"record_type":"book_event","session_date":"2026-08-24","sequence":1,"source_offset":339,"timestamp_ns":1108152157446,"stock_locate":1,"symbol":"AAPL","message_type":"F","action":"add","side":"S","order_ref":1230066625199609624,"new_order_ref":null,"event_qty":200,"display_price4":1234600,"execution_price4":null,"match_number":null,"session_state":"market_hours","trading_state":"T","two_sided":true,"locked":false,"crossed":false,"bids":[[1234500,100]],"asks":[[1234600,200]]}
{"record_type":"summary","schema":"lobforge.book_event","version":1,"input_bytes":810,"records_seen":28,"records_decoded":28,"records_applied":28,"records_output":7,"records_skipped":0,"errors":0,"factual_book_digest":"d3f11ab4fe308743"}
```
