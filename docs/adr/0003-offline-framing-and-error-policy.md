# ADR 0003: Offline framing and strict/permissive error policy

- Status: Accepted
- Date: 2026-08-25

## Context

ITCH application payloads are normally carried by an external transport. Round 2 intentionally
excludes SoupBinTCP, compressed SoupBinTCP, MoldUDP64, GLIMPSE, sockets, packet capture, and recovery,
but needs a deterministic, license-safe offline replay format and precise malformed-input behavior.

## Decision

LOBForge historical files contain repeated records:

```text
u16 big-endian payload_length | payload_length bytes of ITCH application message
```

The two-byte envelope is a LOBForge convention, not part of an ITCH 5.0 application message. Payload
integers are decoded explicitly in big-endian order. No packed structs, unaligned loads, host-endian
casts, or lifetime-escaping views are permitted.

Every diagnostic contains a stable category, absolute file byte offset, zero-based record index,
message type when known, expected/actual lengths when relevant, and stable presentation text.

- Strict mode stops on the first framing, decoding, enum, or semantic error and reports failure.
- Permissive mode counts and reports a bad, completely framed record and continues at the next known
  envelope boundary. It never repairs or clamps data.
- An incomplete two-byte envelope or payload overrun at EOF is terminal in both modes because no safe
  next boundary exists.

The decoder and reconstructor perform no file, console, network, logging, or wall-clock I/O. The CLI
owns file reads and deterministic text/JSON presentation. Paths and timings never enter canonical
state or its FNV-1a 64-bit digest.

## Consequences

Offline files are easy to generate and slice while the application decoder remains transport-neutral.
Permissive replay can make progress through isolated, well-framed bad records, yet truncated input
cannot silently desynchronize. Stable categories support tests and operational aggregation without
parsing human prose.

## Rejected alternatives

- Guessing a next message boundary after a truncated frame was rejected as unsafe.
- Treating a length prefix as part of the ITCH payload was rejected as a protocol category error.
- Resynchronizing by searching for known type bytes was rejected because arbitrary field bytes can
  equal valid message types.
- Exceptions carrying free-form strings only were rejected because callers need stable categories.
