# `lobforge_mm_sim` CLI

## Usage

```sh
lobforge_mm_sim \
  --input <framed-itch-file> \
  --config configs/round4_protocol.toml \
  --output-dir artifacts/round4/session
```

The license-safe fixture replaces `--input` with `--synthetic-fixture primary`. Exactly one source
is required. `--version` prints `lobforge_mm_sim_v1`. Usage errors return 2; I/O/config/publication
errors return 3; framing/decoding/factual-apply errors return 4; invariant failures return 5.
Successful stdout contains only `semantic_digest=<hex>`. Diagnostics use stderr only.
`--event-chunk N` controls sequential orchestration batching (default 65,536) without changing
event order; chunk sizes 1 and 1,024 are golden-tested to produce the same semantic digest.

The CLI validates and hashes the parsed complete protocol, replays facts strictly, checks factual
and shadow invariants after each message, and publishes atomically from a sibling temporary
directory. It refuses to overwrite an existing output directory and never skips a malformed fact.

Output files are:

- `protocol.json`: normalized parsed values and SHA-256;
- `shadow_orders.ndjson`, `shadow_fills.ndjson`, `inventory_events.ndjson`: versioned audits;
- `summary.json` and `metrics.json`: deterministic experiment summary;
- `manifest.json`: source SHA-256/size, protocol and replay digests, row counts, simulator version,
  commit (`unknown` when unavailable), evidence status and semantic digest.

Paths, system time and temporary names are not included in semantic state. The input manifest stores
only the source basename, never a private absolute local path. The CLI has no network, broker,
order-entry or live-trading capability.
