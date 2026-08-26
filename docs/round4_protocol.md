# Round 4 frozen protocol

The normative file is [`configs/round4_protocol.toml`](../configs/round4_protocol.toml). The C++
loader accepts scalar and array TOML assignments, rejects duplicate/malformed/missing keys,
normalizes all parsed `(section.key, value)` pairs in sorted order, and computes SHA-256 over that
complete canonical mapping. Whitespace and comments are non-semantic; every parsed option is
included, including sensitivity and evidence-language fields.

The protocol freezes nanosecond market/compute/order/cancel/replace latencies, the conservative MBO
FIFO primary queue, optimistic/pessimistic sensitivities, fee/rebate units, quote size/rest/refresh/
distance/tick settings, hard risk limits, all three strategy settings, `train`-only calibration,
10ms/100ms/1s markouts, conservative liquidation, seed, complete-date chronological split, maximum
horizon purge, audit schema versions and factual-first timestamp ties.

The checked-in synthetic preset is not a claim that its gamma, volatility, intensity, fee or signal
coefficient describes a real venue. A real experiment must retain the protocol rules while replacing
only train-fitted values through an auditable train-only artifact. Test/validation refitting is
explicitly false.
