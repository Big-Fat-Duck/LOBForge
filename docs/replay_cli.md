# `lobforge_replay` CLI reference

## Input convention

The input is an offline sequence of records:

```text
u16 big-endian payload_length | payload_length bytes of ITCH application payload
```

The two-byte length is a LOBForge file envelope, not part of Nasdaq ITCH. Transport protocols,
compression, recovery, and live sockets are out of scope.

## Usage

```text
lobforge_replay --input FILE [--strict|--permissive]
                [--symbol SYMBOL] [--top N]
                [--format text|json] [--max-messages N]

lobforge_replay --input FILE --session-date YYYY-MM-DD
                [--strict|--permissive] [--symbol SYMBOL ...]
                --research-format book-event-v1 [--depth 1..10]
                --output -|FILE
```

- `--input FILE` is required.
- `--strict` stops on the first framing, decode, enum, or semantic error. It is the default.
- `--permissive` counts and skips a bad, fully framed record when the next boundary is safe.
  `--strict` and `--permissive` are mutually exclusive.
- `--symbol SYMBOL` selects a trimmed directory symbol for depth output.
- `--top N` limits levels per side; the default is 10.
- `--format text|json` selects deterministic output; the default is text.
- `--max-messages N` stops after exactly N records for a deterministic prefix replay.

The second form is the Round 3 research exporter. `--session-date` is mandatory because an ITCH
timestamp does not carry a calendar date. `--symbol` is repeatable, `--depth` defaults to 10, and
`--output -` writes NDJSON to stdout. All progress, warnings and errors remain on stderr. The
exporter emits one atomic post-event record for every successfully applied `A/F/E/C/X/D/U`
mutation; `P/Q/B` never create displayed-book rows. See `docs/book_event_v1.md` for the fixed key
order, nullable fields, error semantics and compatibility rules.

The entire file is read into memory once. The parser/reconstructor itself performs no I/O.

## Output schema

Both formats contain input bytes; records seen, decoded, applied, skipped, and failed; per-type
counts; first/last nanosecond timestamp; session phase; directory symbol count; active orders;
nonempty price levels; displayed add/cancel/execute volume; printable, non-printable, and broken
trade volume; warning/error counts by stable category; the hexadecimal FNV-1a 64-bit state digest;
and optional top-N bid/ask levels (`price`, aggregate `shares`, FIFO `orders`).

JSON uses schema name `lobforge_replay_v1`, decimal integers, a 16-digit lowercase hexadecimal
digest string, deterministic lexicographic message/category ordering, and deterministic key order.
It contains no path, pointer, elapsed time, or wall-clock value.

## Error policy and diagnostics

Every parse or semantic error carries a stable category, absolute file offset, zero-based record
index, message type when available, expected/actual lengths when relevant, and stable diagnostic
text. Permissive mode never invents, clamps, or repairs a field. Incomplete two-byte envelopes and
payloads extending beyond EOF terminate both modes.

## Exit codes

| Code | Meaning | Tested case |
|---:|---|---|
| 0 | Successful replay | full-session text/JSON |
| 2 | Invalid CLI usage | missing `--input` |
| 3 | Input file open/read failure | nonexistent input |
| 4 | Strict parse or semantic failure | malformed framed record |
| 5 | Internal invariant failure | protected by direct invariant tests and runtime checks |

## Canonical state and digest

`Replayer::canonical_state()` serializes fixed ASCII labels and decimal integer values in a fixed
byte order. It includes session phase, sorted day-local directory/reference state, sorted symbols,
bids descending, asks ascending, FIFO order details, typed administrative state, trade ledger, and
deterministic counters. It excludes input paths, addresses, hash-table iteration order, timings,
and wall time. `Replayer::digest()` applies non-cryptographic FNV-1a 64-bit to those bytes. This is
an equality/regression fingerprint, not a security checksum.

The checked-in full-session fixture's digest is `eaa0ddd8309c94c0`.
